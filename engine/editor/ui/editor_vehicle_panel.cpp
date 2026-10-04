#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/logic/editor_world.h>
#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <cmath>
#include <cstdlib>

namespace me
{

namespace
{
const char* GetGearLabel(int gear)
{
    static constexpr const char* kForwardGears[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    if (gear < 0)
    {
        return "R";
    }
    if (gear == 0)
    {
        return "N";
    }
    return gear <= 8 ? kForwardGears[gear - 1] : "8+";
}

void DrawTelemetry(const VehicleDriveStatus& status)
{
    const VehicleTelemetry& telemetry = status.telemetry;
    ImGui::Text("Speed: %.0f km/h", std::abs(telemetry.forwardSpeed) * 3.6f);
    ImGui::Text("Engine: %.0f rpm   Gear: %s", telemetry.engineRpm, GetGearLabel(telemetry.gear));
    ImGui::Text("Wheels on the ground: %u of 4", telemetry.wheelsInContact);
    if (status.realTimeShare < 0.95f)
    {
        // The physics can't keep up: the drive is in slow motion (a debug build runs the multibody car
        // and its tyres many times slower than a release one).
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Physics at %.0f%% of real time (slow motion)", status.realTimeShare * 100.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The physics steps at 1000 Hz and gets at most 25 ms a frame. A Debug build runs the\nmultibody suspension and the brush tyres 15-20 times slower than Release: use the Release build to drive.");
        }
    }
    ImGui::TextDisabled(
        "Collision: %zu static bodies, %zu triangles",
        status.staticBodyCount,
        status.staticTriangleCount);
    if (!status.carData.empty())
    {
        ImGui::TextDisabled("Car's own data: %s", status.carData.c_str());
    }
    if (!status.wheels.empty() && status.wheels.front().brushTyre)
    {
        // The brush tyres at work: each wheel's forces, how much of its patch slides, and how far its
        // carcass has shifted and twisted against the rim.
        ImGui::SeparatorText("Brush tyres (flexible carcass)");
        static constexpr const char* kWheelNames[] = {"FL", "FR", "RL", "RR"};
        if (ImGui::BeginTable("BrushTyres", 8, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            for (const char* column : {"", "Load N", "Fx N", "Fy N", "Mz Nm", "Slide %", "Carcass x/y mm", "Twist deg"})
            {
                ImGui::TableSetupColumn(column);
            }
            ImGui::TableHeadersRow();
            for (size_t index = 0; index < status.wheels.size() && index < 4; ++index)
            {
                const VehicleWheelState& wheel = status.wheels[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kWheelNames[index]);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", wheel.inContact ? wheel.suspensionForce : 0.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", wheel.longitudinalForce);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", -wheel.lateralForce);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", wheel.aligningTorque);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", wheel.slidingShare * 100.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f / %.1f", wheel.carcassDeflection.x * 1000.0f, wheel.carcassDeflection.y * 1000.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", wheel.carcassDeflection.z * 57.2958f);
            }
            ImGui::EndTable();
        }
    }
}

// The fields a user tunes; the geometry is fitted to the model when driving starts.
void DrawTuning(VehicleSettings& tuning)
{
    int front = static_cast<int>(tuning.modelFront);
    static constexpr const char* kFrontLabels[] = {"-Z (Assetto Corsa import)", "+Z (glTF convention)"};
    if (ImGui::Combo("Model Front", &front, kFrontLabels, IM_ARRAYSIZE(kFrontLabels)))
    {
        tuning.modelFront = static_cast<VehicleModelFront>(front);
    }

    int tyreModel = static_cast<int>(tuning.tyreModel);
    static constexpr const char* kTyreLabels[] = {"Physics engine (slip curves)", "Brush, flexible carcass"};
    if (ImGui::Combo("Tyre Model", &tyreModel, kTyreLabels, IM_ARRAYSIZE(kTyreLabels)))
    {
        tuning.tyreModel = static_cast<VehicleTyreModel>(tyreModel);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Physics engine: Jolt's friction curves of slip ratio and slip angle, each direction on its own.\n"
            "Brush: bristles over each rib's contact patch on a carcass that shifts, bends and twists against the rim\n"
            "(Stocco, Biral & Bertolazzi 2024). Grip is shared between braking and cornering, the force builds over\n"
            "the carcass's relaxation length, and the aligning moment comes from the patch. Takes effect on the next drive.");
    }

    ImGui::Checkbox("Use the Car's Own Data", &tuning.useCarData);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "A car imported from Assetto Corsa carries its data.acd's mass, drive, engine, gearbox, steering,\n"
            "brakes and springs. On, they replace the fields below they cover; off, only the fields below count.");
    }
    DragFloatInRange("Mass (kg)", &tuning.massKg, 200.0f, 5000.0f, "%.0f", 5.0f);
    DragFloatInRange("Engine Torque (Nm)", &tuning.maxEngineTorque, 50.0f, 2000.0f, "%.0f", 5.0f);
    DragFloatInRange("Max RPM", &tuning.maxRpm, 3000.0f, 12000.0f, "%.0f", 25.0f);
    int drive = static_cast<int>(tuning.drive);
    static constexpr const char* kDriveLabels[] = {"Rear-wheel drive", "Front-wheel drive", "All-wheel drive"};
    if (ImGui::Combo("Drive", &drive, kDriveLabels, IM_ARRAYSIZE(kDriveLabels)))
    {
        tuning.drive = static_cast<VehicleDrive>(drive);
    }
    DragFloatInRange("Max Steer (deg)", &tuning.maxSteerAngleDegrees, 5.0f, 60.0f, "%.1f", 0.25f);
    DragFloatInRange("Brake Torque (Nm)", &tuning.maxBrakeTorque, 0.0f, 10000.0f, tuning.maxBrakeTorque > 0.0f ? "%.0f" : "Auto", 10.0f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Per wheel. Auto (0) sizes the brakes to the car's weight and tyres so that they stop it hard without locking the wheels.");
    }
    ImGui::Checkbox("Brake Torque by Load", &tuning.dynamicBrakeBias);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "On, the four wheels share the brakes' total torque by the load each carries at the moment (braking\n"
            "moves weight onto the front, a wheel in the air gets none); the car's own front/rear split is not used.\n"
            "Off, the torque goes by that fixed split.");
    }
    DragFloatInRange("Hand Brake Torque (Nm)", &tuning.maxHandBrakeTorque, 0.0f, 10000.0f, "%.0f", 10.0f);
    DragFloatInRange("Spring Frequency (Hz)", &tuning.suspensionFrequencyHz, 0.5f, 5.0f, "%.2f", 0.01f);
    DragFloatInRange("Spring Damping", &tuning.suspensionDamping, 0.0f, 2.0f, "%.2f", 0.01f);
    DragFloatInRange("Traction Control (grip)", &tuning.tractionControlGrip, 0.0f, 1.5f, tuning.tractionControlGrip > 0.0f ? "%.2f" : "Off", 0.01f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The clutch slips once the engine asks the driven wheels for more than their tyres can hold: this share of\n"
            "their peak grip on the load they carry. 1 is the limit; more lets them spin up. Off (0) leaves a car at\n"
            "full throttle in a low gear spinning its tyres several times the ground's speed.");
    }
    ImGui::Checkbox("Anti-roll Bars", &tuning.antiRollBars);
    ImGui::SameLine();
    ImGui::Checkbox("Limited-slip Differentials", &tuning.limitedSlipDifferentials);
    if (ImGui::Button("Defaults"))
    {
        tuning = VehicleDriveService::DefaultTuning();
    }
}
}

void EditorUiController::DrawVehiclePanel(const IEditorWorld& scene, EditorUiFrameResult& result)
{
    if (!ImGui::Begin("Vehicle", &m_showVehicleWindow))
    {
        ImGui::End();
        return;
    }

    const VehicleDriveStatus& status = m_vehicleStatus;
    if (status.active)
    {
        ImGui::Text("%s %s: %s", ICON_FA_CAR, status.paused ? "Paused" : "Driving", status.vehicleName.c_str());
        if (ImGui::Button(ICON_FA_STOP " Stop"))
        {
            result.actions.stopVehicleDrive = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(status.paused ? ICON_FA_PLAY " Resume" : ICON_FA_PAUSE " Pause"))
        {
            result.actions.pauseVehicleDrive = !status.paused;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset Car"))
        {
            result.actions.resetVehicle = true;
        }
        ImGui::Separator();
        DrawTelemetry(status);
    }
    else
    {
        const bool canDrive = scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity());
        ImGui::BeginDisabled(!canDrive);
        if (ImGui::Button(ICON_FA_PLAY " Drive Selected Model"))
        {
            result.actions.startVehicleDrive = true;
        }
        ImGui::EndDisabled();
        if (!canDrive)
        {
            ImGui::TextDisabled("Select the car's model in the scene.");
        }
        else
        {
            ImGui::TextDisabled(
                "Every other model becomes the track. The car's front is %s (Tuning > Model Front).",
                m_vehicleTuning.modelFront == VehicleModelFront::NegativeZ ? "-Z" : "+Z");
        }
    }
    if (ImGui::Button(ICON_FA_CHART_LINE " Suspension Rigs"))
    {
        m_showSuspensionRigWindow = true;
        ImGui::SetWindowFocus("Suspension Rigs");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The virtual K&C and seven-post rigs on the selected car's suspension data, with its linkage animated.");
    }
    if (!status.lastError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status.lastError.c_str());
    }

    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("W/S or Up/Down: throttle, brake and reverse");
        ImGui::TextUnformatted("A/D or Left/Right: steer    Space: hand brake");
        ImGui::TextUnformatted("Backspace: reset the car    F5: stop");
        ImGui::TextUnformatted("Hold the right mouse button: look around the car");
        ImGui::TextDisabled("Gamepad (Xbox or DualSense): RT/LT, left stick, right stick looks around the car, A (Cross) hand brake, Back (Create) reset");
        ImGui::TextDisabled("Click the viewport first: keys typed into a panel do not drive.");
    }

    if (ImGui::CollapsingHeader("Chase Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Follow the Car", &m_vehicleCamera.follow);
        ImGui::BeginDisabled(!m_vehicleCamera.follow);
        DragFloatInRange("Distance (m)", &m_vehicleCamera.distance, 2.0f, 30.0f, "%.1f", 0.05f);
        DragFloatInRange("Height (m)", &m_vehicleCamera.height, 0.2f, 15.0f, "%.1f", 0.05f);
        DragFloatInRange("Look Height (m)", &m_vehicleCamera.lookHeight, 0.0f, 5.0f, "%.1f", 0.05f);
        DragFloatInRange("Stiffness", &m_vehicleCamera.stiffness, 0.5f, 30.0f, "%.1f", 0.1f);
        DragFloatInRange("Look Recentre Rate", &m_vehicleCamera.lookRecenterRate, 0.5f, 20.0f, "%.1f", 0.1f);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Gamepad Feedback", ImGuiTreeNodeFlags_DefaultOpen))
    {
        VehicleHapticsSettings& haptics = m_vehicleHaptics;
        ImGui::Checkbox("Engine Rumble and Gear Thump", &haptics.enabled);
        ImGui::BeginDisabled(!haptics.enabled);
        DragFloatInRange("Rumble Strength", &haptics.rumbleStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        DragFloatInRange("Trigger Strength", &haptics.triggerStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        ImGui::Checkbox("Brake Trigger Shakes When a Wheel Locks", &haptics.brakeLockFeedback);
        ImGui::EndDisabled();
        ImGui::TextDisabled("DualSense: the triggers push back (RT accelerator, LT brake) and shake. Other pads: rumble only.");
    }

    if (ImGui::CollapsingHeader("Physics Overlay", ImGuiTreeNodeFlags_DefaultOpen))
    {
        VehiclePhysicsOverlaySettings& overlay = m_vehicleOverlay;
        ImGui::Checkbox("Show Suspension and Tyre Physics", &overlay.enabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Draws over the viewport while a car is driven, also with the simulation paused.");
        }
        ImGui::BeginDisabled(!overlay.enabled);
        ImGui::Checkbox("Springs", &overlay.suspension);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The spring from its mount to the wheel, and a rail beside the tyre for its travel: the red end is\n"
                "full bump, the far end full droop. The marker turns red as the spring nears its bump stop.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Tyres", &overlay.tyres);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The tread and the contact patch, green to red by how much of the tyre's peak grip its forces\n"
                "use (grey in the air).");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Forces", &overlay.forces);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "At each contact patch, what the ground does to the car: green the load up through the\n"
                "spring, orange the drive and braking along the tyre, blue the cornering across it.");
        }
        ImGui::Checkbox("Friction Circles", &overlay.frictionCircles);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Bottom left: a circle per wheel, its edge the tyre's peak grip. The dot is the tyre's force, along\n"
                "it up and across it to the right. With the load, the slip (ratio and angle), the spring's travel and the brake torque.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Linkage", &overlay.linkage);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The multibody suspension at each wheel, over the body: arms and rods blue, uprights (or a solid\n"
                "axle's beam) orange, chassis pivots grey, the joints on the moving parts yellow.");
        }
        DragFloatInRange("Arrow Length (m/kN)", &overlay.metresPerKilonewton, 0.02f, 1.0f, "%.2f", 0.005f);
        ImGui::Checkbox("Brush Contact Patch", &overlay.contactPatch);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "With the brush tyre: each rib's contact, green where its bristles stick to the road and red where they\n"
                "slide, on the carcass's centre line (yellow), shifted, bent and twisted against the rim. The patch at rest\n"
                "is outlined in grey.");
        }
        DragFloatInRange("Deformation Scale", &overlay.deformationScale, 1.0f, 50.0f, "%.0fx", 0.25f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How many times its size the carcass's deflection is drawn: it is a few millimetres.");
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Tuning"))
    {
        if (status.active)
        {
            ImGui::TextDisabled("Changes apply the next time driving starts.");
        }
        DrawTuning(m_vehicleTuning);
    }

    ImGui::End();
}
}
