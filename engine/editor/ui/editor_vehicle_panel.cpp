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
    ImGui::TextDisabled(
        "Collision: %zu static bodies, %zu triangles",
        status.staticBodyCount,
        status.staticTriangleCount);
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
    DragFloatInRange("Brake Torque (Nm)", &tuning.maxBrakeTorque, 100.0f, 10000.0f, "%.0f", 10.0f);
    DragFloatInRange("Hand Brake Torque (Nm)", &tuning.maxHandBrakeTorque, 0.0f, 10000.0f, "%.0f", 10.0f);
    DragFloatInRange("Spring Frequency (Hz)", &tuning.suspensionFrequencyHz, 0.5f, 5.0f, "%.2f", 0.01f);
    DragFloatInRange("Spring Damping", &tuning.suspensionDamping, 0.0f, 2.0f, "%.2f", 0.01f);
    ImGui::Checkbox("Anti-roll Bars", &tuning.antiRollBars);
    ImGui::SameLine();
    ImGui::Checkbox("Limited-slip Differentials", &tuning.limitedSlipDifferentials);
    if (ImGui::Button("Defaults"))
    {
        tuning = VehicleSettings{};
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
        ImGui::TextDisabled("Gamepad: RT/LT, left stick, A hand brake, Back reset");
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
