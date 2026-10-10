#include "vehicle_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/suspension_rigs_panel.h>

#include <engine/asset/model_cache.h>
#include <engine/editor/services/vehicle_tyre_fitment.h>
#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>
#include <engine/tyre/tyre_brush.h>
#include <IconsPhosphor.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>

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
        ImGui::TextColored(ui_colors::kTextWarning, "Physics at %.0f%% of real time (slow motion)", status.realTimeShare * 100.0f);
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
    if (!status.wheels.empty())
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
    if (std::any_of(telemetry.tyres.begin(), telemetry.tyres.end(), [](const VehicleTelemetry::TyreTemperatures& tyre) { return tyre.simulated; }))
    {
        // The tyres' temperatures (the tread's lanes round the tyre, inside to outside, and the core), their
        // pressure and the grip the two leave them, as the game's thermal model has them.
        ImGui::SeparatorText("Tyre temperatures");
        static constexpr const char* kWheelNames[] = {"FL", "FR", "RL", "RR"};
        if (ImGui::BeginTable("TyreTemperatures", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            for (const char* column : {"", "Tread I / M / O C", "Core C", "Pressure psi", "Grip %"})
            {
                ImGui::TableSetupColumn(column);
            }
            ImGui::TableHeadersRow();
            for (size_t index = 0; index < telemetry.tyres.size(); ++index)
            {
                const VehicleTelemetry::TyreTemperatures& tyre = telemetry.tyres[index];
                if (!tyre.simulated)
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kWheelNames[index]);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f / %.0f / %.0f", tyre.tread[0], tyre.tread[1], tyre.tread[2]);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.core);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.pressure);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.grip * 100.0f);
            }
            ImGui::EndTable();
        }
    }
    if (std::any_of(telemetry.wear.begin(), telemetry.wear.end(), [](const VehicleTelemetry::TyreWear& tyre) { return tyre.simulated; }))
    {
        ImGui::SeparatorText("Tyre wear");
        static constexpr const char* kWheelNames[] = {"FL", "FR", "RL", "RR"};
        if (ImGui::BeginTable("TyreWear", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            for (const char* column : {"", "Slid km", "Grain %", "Blister %", "Grip %"})
            {
                ImGui::TableSetupColumn(column);
            }
            ImGui::TableHeadersRow();
            for (size_t index = 0; index < telemetry.wear.size(); ++index)
            {
                const VehicleTelemetry::TyreWear& tyre = telemetry.wear[index];
                if (!tyre.simulated)
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kWheelNames[index]);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", tyre.virtualKm);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.grain);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.blister);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", tyre.grip * 100.0f);
            }
            ImGui::EndTable();
        }
    }
}

// ABS and traction control on or off. While a car is driven the switches are its own (the keys and the
// D-pad switch them too) and a change here reaches it at once; the next drive starts as they are left.
void DrawDriverAids(VehicleSettings& tuning, const VehicleDriveStatus& status, EditorUiFrameResult& result)
{
    if (status.active)
    {
        tuning.useAbs = status.absOn;
        tuning.useTractionControl = status.tractionControlOn;
    }
    bool changed = ImGui::Checkbox("ABS", &tuning.useAbs);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Anti-lock brakes, for a car whose data has them (its slip limit and rate): a wheel turning\n"
                          "slower than the road by more than the limit has its brake let off until it is back under.\n"
                          "B or the D-pad's left while driving.");
    }
    if (status.active && !status.absFitted)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(this car has none)");
    }
    changed |= ImGui::Checkbox("Traction Control", &tuning.useTractionControl);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The car's own traction control when its data has one (the throttle is cut while a driven wheel\n"
                          "spins past its slip limit), else the clutch slipping at Tuning > Traction Control (grip).\n"
                          "T or the D-pad's right while driving.");
    }
    if (status.active && !status.tractionControlFitted)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(this car has none)");
    }
    if (changed && status.active)
    {
        result.actions.driverAids = std::array<bool, 2>{tuning.useAbs, tuning.useTractionControl};
    }
}

// How many fixed steps the physics takes a simulated second, which a car being driven takes at once.
void DrawPhysicsRate(int& rateHz)
{
    const int minRate = static_cast<int>(std::lround(1.0f / PhysicsWorld::kMaxStepSeconds));
    const int maxRate = static_cast<int>(std::lround(1.0f / PhysicsWorld::kMinStepSeconds));
    DragIntInRange("Physics Rate (Hz)", &rateHz, minRate, maxRate);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "How many fixed steps the physics takes per simulated second (1000 by default; Assetto Corsa runs 333).\n"
            "Faster follows stiff springs, kerbs and the tyres' contact more closely, at a cost that grows in step with\n"
            "the rate (a world too slow for real time runs in slow motion); slower is cheaper but a stiff car or the\n"
            "brush tyre can ring or go unstable. Applies at once, also while driving.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.3f ms", 1000.0f / static_cast<float>(std::max(rateHz, 1)));
    static constexpr int kPresets[] = {333, 500, 1000, 2000};
    for (const int preset : kPresets)
    {
        if (preset != kPresets[0])
        {
            ImGui::SameLine();
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%d##rate", preset);
        if (ImGui::SmallButton(label))
        {
            rateHz = preset;
        }
    }
}

// The fields a user tunes; the geometry is fitted to the model when driving starts. True when the
// brush tyre's rib count changed, which a car being driven takes at once.
bool DrawTuning(VehicleSettings& tuning)
{
    bool ribsChanged = false;
    int front = static_cast<int>(tuning.modelFront);
    static constexpr const char* kFrontLabels[] = {"-Z (Assetto Corsa import)", "+Z (glTF convention)"};
    if (ImGui::Combo("Model Front", &front, kFrontLabels, IM_ARRAYSIZE(kFrontLabels)))
    {
        tuning.modelFront = static_cast<VehicleModelFront>(front);
    }

    int ribs = tuning.brushTyreRibs > 0 ? tuning.brushTyreRibs : tyre::BrushTyreParameters{}.ribs;
    if (DragIntInRange("Brush Ribs", &ribs, 1, tyre::kBrushMaxRibs))
    {
        tuning.brushTyreRibs = ribs;
        ribsChanged = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "How many ribs each brush tyre is cut into across its tread, each a row of bristles over its own contact\n"
            "length. More follow camber and the pressure across the tread more finely; the tyres' cost grows about in\n"
            "step with ribs times segments (10 x 20 is about 0.15 s of physics per simulated second for one car in\n"
            "Release). Applies at once, also while driving.");
    }
    int segments = tuning.brushTyreSegments > 0 ? tuning.brushTyreSegments : tyre::BrushTyreParameters{}.segmentsPerRib;
    if (DragIntInRange("Brush Segments", &segments, 2, tyre::kBrushMaxSegments))
    {
        tuning.brushTyreSegments = segments;
        ribsChanged = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "How many segments each rib's contact is cut into along its length, the bristles tracked at each one's\n"
            "ends. More place where the bristles start to slide more finely; the cost grows in step, as with the ribs.\n"
            "Applies at once, also while driving.");
    }
    DragFloatInRange("Belt Inertia Share", &tuning.beltInertiaShare, 0.05f, 0.95f, "%.2f", 0.01f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Each wheel turns as a rim (with the hub, brake disc and half-shaft end) and the tyre's belt on its\n"
            "sidewalls: the belt's share of the wheel's spin inertia. 0.5 by SWIFT's belt for a car tyre of the R34's\n"
            "size (Pacejka 2006). Takes effect on the next drive.");
    }
    DragFloatInRange("Sidewall Twist Damping", &tuning.sidewallTorsionDampingRatio, 0.0f, 0.5f, "%.3f", 0.001f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The sidewalls' torsional damping rolling, as a share of critical with the rim held (the belt's own ring,\n"
            "some 50 to 70 Hz). 0.03 by Massaro et al. (2023): a car tyre's torsional mode at 3.4 %. Standing still it\n"
            "rises with the carcass's own damping. Takes effect on the next drive.");
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
            "full throttle in a low gear spinning its tyres several times the ground's speed. A car whose data has\n"
            "traction control uses that instead. Switched on and off by Controls > Traction Control.");
    }
    ImGui::Checkbox("Anti-roll Bars", &tuning.antiRollBars);
    ImGui::SameLine();
    ImGui::Checkbox("Limited-slip Differentials", &tuning.limitedSlipDifferentials);
    VehicleDrivetrainLosses& losses = tuning.drivetrainLosses;
    ImGui::Checkbox("Drivetrain Losses", &losses.enabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The gears' meshes pass only a share of the torque (the efficiencies below; driving the wheels get that share,\n"
            "on the overrun they must give the engine's drag over it), and the oil and bearings drag each axle the\n"
            "drivetrain turns. Assetto Corsa has none: its torque curves are already the wheels', so with this on the\n"
            "engine makes the curve over the indirect gears' efficiency, and the wheels get the game's torque in those.");
    }
    ImGui::BeginDisabled(!losses.enabled);
    DragFloatInRange("Gearbox Efficiency", &losses.gearboxEfficiency, 0.8f, 1.0f, "%.3f", 0.001f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("An indirect gear: two meshes through the layshaft.");
    }
    DragFloatInRange("Direct Gear Efficiency", &losses.directGearEfficiency, 0.8f, 1.0f, "%.3f", 0.001f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("The gear of ratio 1: straight through the main shaft, only bearings and seals.");
    }
    DragFloatInRange("Final Drive Efficiency", &losses.finalDriveEfficiency, 0.8f, 1.0f, "%.3f", 0.001f);
    DragFloatInRange("Transfer Case Efficiency", &losses.transferEfficiency, 0.8f, 1.0f, "%.3f", 0.001f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Four-wheel drive: all the torque with a centre differential, what a coupling passes to the front.");
    }
    DragFloatInRange("Spin Drag (Nm)", &losses.spinTorque, 0.0f, 50.0f, "%.1f", 0.1f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Each axle the drivetrain turns, at the wheels, whatever the load: the bearings and seals.");
    }
    DragFloatInRange("Spin Drag per rad/s (Nm)", &losses.spinTorquePerSpeed, 0.0f, 1.0f, "%.3f", 0.001f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Each axle's drag growing with the wheels' speed: the oil churning. At 250 km/h a wheel turns about 210 rad/s.");
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Defaults"))
    {
        tuning = VehicleDriveService::DefaultTuning();
        ribsChanged = true;
    }
    return ribsChanged;
}
}

void VehiclePanel::DrawTyres(const IEditorWorld& scene, EditorVehicleSettings& vehicle, bool driving)
{
    ImGui::Checkbox("Tyre Wear", &vehicle.tuning.tyreWear);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The tyres wear with sliding (WEAR_CURVE over the km slid) and, with temperatures on, grain when too cold and\n"
            "blister when too hot, as Assetto Corsa has them. Each drive starts on new tyres; takes effect on the next drive.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Tyre Temperatures", &vehicle.tuning.tyreTemperatures);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The tyres warm with sliding and rolling and cool with speed, and their grip and pressure follow, as Assetto\n"
            "Corsa's thermal model has them (the performance curve, PRESSURE_IDEAL, PRESSURE_D_GAIN ...). Off, they stay\n"
            "at their best grip and ideal pressure. Brush tyres only; takes effect on the next drive.");
    }
    ImGui::BeginDisabled(!vehicle.tuning.tyreTemperatures);
    DragFloatInRange("Start Temperature", &vehicle.tuning.tyreStartTemperature, -20.0f, 120.0f, "%.0f C", 1.0f);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("What the tyres start at: the game's air and road are 26 C; warm tyres (blankets) are 80 C.");
    }
    std::string path;
    if (scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity()))
    {
        path = scene.GetModel(scene.GetSelectedEntity()).sourcePath;
    }
    const std::shared_ptr<const LoadedModelData> model = path.empty() ? nullptr : ModelCache::Get(path);
    if (!model || !model->carSpec.has_value() || std::none_of(model->carSpec->wheelTyres.begin(), model->carSpec->wheelTyres.end(), [](const auto& tyre) { return tyre.has_value(); }))
    {
        ImGui::TextDisabled("Select a car that carries its own tyre data (an Assetto Corsa import).");
        return;
    }
    const VehicleCarSpec& spec = *model->carSpec;
    std::array<VehicleTyreRef, kVehicleWheelCount>& fitment = vehicle.tuning.tyreFitment;
    if (vehicle.tyreFitmentModel != path)
    {
        fitment = {};
        vehicle.tyreFitmentModel = path;
    }
    if (!m_tyreLibraryLoaded)
    {
        m_tyreLibrary = TyreLibrary::List();
        m_tyreLibraryLoaded = true;
    }

    const auto findEntry = [&](const VehicleTyreRef& ref) -> const TyreLibrary::Entry*
    {
        for (const TyreLibrary::Entry& entry : m_tyreLibrary)
        {
            if ((!ref.uuid.empty() && entry.ref.uuid == ref.uuid) || (ref.uuid.empty() && !ref.path.empty() && entry.ref.path == ref.path))
            {
                return &entry;
            }
        }
        return nullptr;
    };
    // "Semislicks  ks_mazda_rx7_tuned/semislicks_front": the compound, then where it sits in the library.
    const auto labelFor = [](const TyreLibrary::Entry& entry)
    {
        std::string stem = entry.path.filename().string();
        stem.resize(stem.size() - TyreLibrary::kSuffix.size());
        return entry.spec.name + "  " + entry.path.parent_path().filename().string() + "/" + stem;
    };
    const auto tooltipFor = [](const tyre::TyreSpec& tyre)
    {
        const auto number = [](const std::optional<float>& value, const char* format)
        {
            if (!value.has_value())
            {
                return std::string("-");
            }
            char text[32];
            std::snprintf(text, sizeof(text), format, *value);
            return std::string(text);
        };
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tyre.source.empty() ? tyre.name.c_str() : tyre.source.c_str());
        ImGui::Text("Width %s m, radius %s m, rim %s m", number(tyre.size.width, "%.3f").c_str(), number(tyre.size.radius, "%.4f").c_str(),
                    number(tyre.size.rimRadius, "%.4f").c_str());
        ImGui::Text("Grip %s along, %s across at %s N", number(tyre.grip.longitudinalReference, "%.3f").c_str(),
                    number(tyre.grip.lateralReference, "%.3f").c_str(), number(tyre.grip.referenceLoad, "%.0f").c_str());
        ImGui::Text("Peak at %s deg, %s left past it", number(tyre.slip.frictionLimitAngleDegrees, "%.2f").c_str(),
                    number(tyre.slip.falloffLevel, "%.2f").c_str());
        ImGui::Text("Rate %s N/m, %s psi cold", number(tyre.vertical.rate, "%.0f").c_str(), number(tyre.pressure.staticPsi, "%.1f").c_str());
        ImGui::EndTooltip();
    };
    const auto fit = [&](size_t wheel, const VehicleTyreRef& ref) { VehicleTyreFitment::Fit(fitment, spec, wheel, ref, m_pairAxles); };

    static constexpr const char* kWheelNames[kVehicleWheelCount] = {"Front Left", "Front Right", "Rear Left", "Rear Right"};
    ImGui::Checkbox("Both Wheels of an Axle", &m_pairAxles);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A tyre chosen for one wheel goes on the other wheel of its axle too.");
    }
    for (size_t wheel = 0; wheel < kVehicleWheelCount; ++wheel)
    {
        const bool refitted = !fitment[wheel].Empty();
        const VehicleTyreRef& current = refitted ? fitment[wheel] : spec.wheelTyreRefs[wheel];
        const TyreLibrary::Entry* currentEntry = findEntry(current);
        std::string preview = currentEntry != nullptr ? labelFor(*currentEntry)
                              : spec.wheelTyres[wheel].has_value() ? spec.wheelTyres[wheel]->name + "  (the car's own data)"
                                                                    : std::string("None");
        if (refitted)
        {
            preview = ICON_PH_ARROWS_CLOCKWISE " " + preview;
        }
        ImGui::PushID(static_cast<int>(wheel));
        if (ImGui::BeginCombo(kWheelNames[wheel], preview.c_str()))
        {
            std::vector<const TyreLibrary::Entry*> own;
            if (!spec.libraryCompounds.empty())
            {
                ImGui::SeparatorText("This Car");
                for (const VehicleTyreCompoundRefs& compound : spec.libraryCompounds)
                {
                    const bool bothAxles = compound.front == compound.rear;
                    for (const VehicleTyreRef* ref : {&compound.front, &compound.rear})
                    {
                        if (bothAxles && ref == &compound.rear)
                        {
                            continue;
                        }
                        if (const TyreLibrary::Entry* entry = findEntry(*ref))
                        {
                            own.push_back(entry);
                            const std::string label = compound.name + (bothAxles ? "  (front and rear)" : ref == &compound.front ? "  (front)" : "  (rear)");
                            if (ImGui::Selectable(label.c_str(), entry->ref == current))
                            {
                                fit(wheel, entry->ref);
                            }
                            if (ImGui::IsItemHovered())
                            {
                                tooltipFor(entry->spec);
                            }
                        }
                    }
                }
            }
            ImGui::SeparatorText("Tyre Library");
            for (const TyreLibrary::Entry& entry : m_tyreLibrary)
            {
                if (std::find(own.begin(), own.end(), &entry) != own.end())
                {
                    continue;
                }
                if (ImGui::Selectable(labelFor(entry).c_str(), entry.ref == current))
                {
                    fit(wheel, entry.ref);
                }
                if (ImGui::IsItemHovered())
                {
                    tooltipFor(entry.spec);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    const bool anyFitted = VehicleTyreFitment::Any(fitment);
    // Writes the car's glTF; the spec's reference stays valid (the cache replaces its copy in place).
    const auto save = [&](const auto& write, const char* what)
    {
        try
        {
            write();
            fitment = {};
            m_tyreLibrary = TyreLibrary::List();
            m_tyreStatus = what;
        }
        catch (const std::exception& error)
        {
            m_tyreStatus = std::string("Not saved: ") + error.what();
        }
    };
    ImGui::BeginDisabled(!anyFitted);
    if (ImGui::Button(ICON_PH_FLOPPY_DISK " Save to Car"))
    {
        save([&] { VehicleTyreFitment::Save(path, spec, fitment); }, "The car's glTF now names these tyres.");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Writes the tyres chosen here into the car's glTF, so it starts on them from now on.");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE " Revert"))
    {
        fitment = {};
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_ARROWS_CLOCKWISE " Rescan Library"))
    {
        m_tyreLibrary = TyreLibrary::List();
    }
    // A car imported before the library carries its compounds in its glTF alone.
    if (spec.libraryCompounds.empty() && !spec.tyreCompounds.empty())
    {
        if (ImGui::Button(ICON_PH_PLUS " Add the Car's Tyres to the Library"))
        {
            save([&] { VehicleTyreFitment::Adopt(path, spec, std::filesystem::path(path).stem().string()); },
                 "The car's compounds are in the tyre library, and the car names them.");
        }
    }
    if (driving && anyFitted)
    {
        ImGui::TextDisabled("Tyres fitted here apply the next time driving starts.");
    }
    if (!m_tyreStatus.empty())
    {
        ImGui::TextWrapped("%s", m_tyreStatus.c_str());
    }
}

VehiclePanel::VehiclePanel()
    : EditorPanel("vehicle", "Vehicle", ICON_PH_CAR)
{
}

void VehiclePanel::OnGui(EditorContext& context)
{
    const IEditorWorld& scene = context.scene;
    EditorUiFrameResult& result = context.result;
    EditorVehicleSettings& vehicle = context.state.vehicle;
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    if (status.active)
    {
        ImGui::Text("%s %s: %s", ICON_PH_CAR, status.paused ? "Paused" : "Driving", status.vehicleName.c_str());
        if (ImGui::Button(ICON_PH_STOP " Stop"))
        {
            result.actions.stopVehicleDrive = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(status.paused ? ICON_PH_PLAY " Resume" : ICON_PH_PAUSE " Pause"))
        {
            result.actions.pauseVehicleDrive = !status.paused;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE " Reset Car"))
        {
            result.actions.resetVehicle = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_ARROW_U_UP_LEFT " Flip Upright"))
        {
            result.actions.recoverVehicle = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Puts the car back on its wheels where it is, facing the way it was heading (R / Triangle)");
        }
        ImGui::Separator();
        DrawTelemetry(status);
    }
    else
    {
        const bool canDrive = scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity());
        ImGui::BeginDisabled(!canDrive);
        if (ImGui::Button(ICON_PH_PLAY " Drive Selected Model"))
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
                vehicle.tuning.modelFront == VehicleModelFront::NegativeZ ? "-Z" : "+Z");
        }
    }
    if (ImGui::Button(ICON_PH_CHART_LINE " Suspension Rigs"))
    {
        context.windows.Open<SuspensionRigsPanel>();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The virtual K&C and seven-post rigs on the selected car's suspension data, with its linkage animated.");
    }
    if (!status.lastError.empty())
    {
        ImGui::TextColored(ui_colors::kTextDanger, "%s", status.lastError.c_str());
    }

    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawDriverAids(vehicle.tuning, status, result);
        ImGui::Checkbox("Manual Gearbox", &vehicle.manualGearbox);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Sequential, with an automatic clutch: you change gear, through neutral between first and reverse.\n"
                "The throttle only drives and the brake only brakes; reverse goes in once the car has (nearly) stopped,\n"
                "and a change down that would over-rev the engine is refused. Holding the clutch (or the hand brake) opens it;\n"
                "let go, it bites: rev the engine with it held and let go to launch or kick the car out.\n"
                "Off: the automatic picks the gear and pulling back reverses once stopped. Its paddles (E/Q, R1/L1) still\n"
                "change gear, as a tiptronic's: it then holds your gear (M on the HUD) until 8 s pass without a change,\n"
                "changing up itself only on the limiter and down only where the engine would labour.");
        }
        ImGui::TextUnformatted(vehicle.manualGearbox ? "W/S or Up/Down: throttle and brake" : "W/S or Up/Down: throttle, brake and reverse");
        ImGui::TextUnformatted("A/D or Left/Right: steer    Space: hand brake");
        ImGui::TextUnformatted(vehicle.manualGearbox ? "E/Q: change up/down    N (held): clutch" : "E/Q: change up/down (holds the gear for 8 s)");
        ImGui::TextUnformatted("Backspace: reset the car    R: flip upright where it is    F5: stop");
        ImGui::TextUnformatted("V: change view (chase, cockpit, bonnet, bumper)");
        ImGui::TextUnformatted("B: ABS on/off    T: traction control on/off");
        ImGui::TextUnformatted("Hold the right mouse button: look around the car, or turn your head from inside it");
        ImGui::TextDisabled("Gamepad (DualSense / Xbox): R2/L2 (RT/LT), left stick, right stick looks around,");
        ImGui::TextDisabled("Circle (B) hand brake, R1/L1 (RB/LB) change up/down, Cross (A, held) clutch,");
        ImGui::TextDisabled("Create (Back) reset, Triangle (Y) flip upright where it is, R3 (right stick click) change view,");
        ImGui::TextDisabled("D-pad left ABS on/off, D-pad right traction control on/off");
        ImGui::TextDisabled("Click the viewport first: keys typed into a panel do not drive.");
    }

    if (ImGui::CollapsingHeader("Tyres", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawTyres(scene, vehicle, status.active);
    }

    if (ImGui::CollapsingHeader("Steering Assist", ImGuiTreeNodeFlags_DefaultOpen))
    {
        VehicleSteeringAssistSettings& assist = vehicle.steeringAssist;
        ImGui::Checkbox("Smooth Steering (GT7 style)", &assist.enabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The stick or keys ask for a share of the lock the car can use at its speed, not of the rack's full lock;\n"
                "the front wheels follow at a limited rate; and when the car slides, the range centres on where it is\n"
                "going, so letting go of the stick points the wheels along the slide. Off: the stick drives the rack directly.");
        }
        ImGui::BeginDisabled(!assist.enabled);
        DragFloatInRange("Stick Response Curve", &assist.sensitivity, 0.0f, 1.0f, "%.2f", 0.01f);
        DragFloatInRange("Steer Time (s)", &assist.steerSeconds, 0.0f, 1.0f, "%.2f", 0.005f);
        DragFloatInRange("Return Time (s)", &assist.returnSeconds, 0.0f, 1.0f, "%.2f", 0.005f);
        DragFloatInRange("Smoothing (s)", &assist.smoothingSeconds, 0.0f, 0.3f, "%.3f", 0.001f);
        DragFloatInRange("Full Lock Below (m/s)", &assist.fullLockSpeed, 0.5f, 20.0f, "%.1f", 0.05f);
        ImGui::Checkbox("Speed-Sensitive Lock", &assist.speedSensitive);
        ImGui::BeginDisabled(!assist.speedSensitive);
        DragFloatInRange("Corner Grip (g)", &assist.cornerGrip, 0.3f, 3.0f, "%.2f", 0.01f);
        DragFloatInRange("Least Lock Share", &assist.minLockShare, 0.0f, 1.0f, "%.2f", 0.005f);
        ImGui::EndDisabled();
        ImGui::Checkbox("Front Slip Limit", &assist.slipLimit);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Reads the car every frame: the front wheels stay within the front tyres' peak slip angle of the way\n"
                "the front axle is travelling, so they cannot be turned past grip into a push (understeer).\n"
                "Fades in from Full Lock Below to twice that speed.");
        }
        ImGui::BeginDisabled(!assist.slipLimit);
        DragFloatInRange("Peak Slip Share", &assist.slipLimitShare, 0.5f, 2.0f, "%.2f", 0.005f);
        ImGui::EndDisabled();
        ImGui::Checkbox("Counter-Steer Assist", &assist.counterSteerAssist);
        ImGui::BeginDisabled(!assist.counterSteerAssist);
        DragFloatInRange("Slide Dead Zone (deg)", &assist.counterSteerDeadZoneDegrees, 0.0f, 10.0f, "%.1f", 0.05f);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Follow the Car", &vehicle.camera.follow);
        ImGui::BeginDisabled(!vehicle.camera.follow);
        static const char* const kViewLabels[] = {"Chase", "Cockpit", "Bonnet", "Bumper"};
        static_assert(IM_ARRAYSIZE(kViewLabels) == kVehicleCameraViewCount);
        int view = static_cast<int>(status.cameraView);
        if (ImGui::Combo("View", &view, kViewLabels, IM_ARRAYSIZE(kViewLabels)))
        {
            result.actions.vehicleCameraView = static_cast<VehicleCameraView>(view);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "As GT7's views (V or R3 goes to the next): behind the car; the driver's eyes; over the bonnet; on the front bumper.\n"
                "The cockpit, bonnet and bumper cameras are fixed to the body and pitch and roll with it.\n"
                "The driver's eyes come from an Assetto Corsa car's DRIVEREYES, else from the steering wheel (STEER_HR), else the car's size.");
        }
        ImGui::SeparatorText("Chase");
        DragFloatInRange("Distance (m)", &vehicle.camera.distance, 2.0f, 30.0f, "%.1f", 0.05f);
        DragFloatInRange("Height (m)", &vehicle.camera.height, 0.2f, 15.0f, "%.1f", 0.05f);
        DragFloatInRange("Look Height (m)", &vehicle.camera.lookHeight, 0.0f, 5.0f, "%.1f", 0.05f);
        DragFloatInRange("Look Recentre Rate", &vehicle.camera.lookRecenterRate, 0.0f, 20.0f, "%.1f", 0.1f);
        DragFloatInRange("Chase FOV (deg)", &vehicle.camera.chaseFovDegrees, 30.0f, 100.0f, "%.0f", 0.25f);
        ImGui::SeparatorText("Cockpit, Bonnet and Bumper");
        DragFloatInRange("Cockpit FOV (deg)", &vehicle.camera.cockpitFovDegrees, 30.0f, 100.0f, "%.0f", 0.25f);
        DragFloatInRange("Bonnet and Bumper FOV (deg)", &vehicle.camera.exteriorFovDegrees, 30.0f, 100.0f, "%.0f", 0.25f);
        DragFloatInRange("Seat Right (m)", &vehicle.camera.seatOffset.x, -0.5f, 0.5f, "%.2f", 0.005f);
        DragFloatInRange("Seat Up (m)", &vehicle.camera.seatOffset.y, -0.5f, 0.5f, "%.2f", 0.005f);
        DragFloatInRange("Seat Forward (m)", &vehicle.camera.seatOffset.z, -0.5f, 0.5f, "%.2f", 0.005f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Moves the driver's eyes in the cockpit view from where the car's data or its steering wheel puts them.");
        }
        DragFloatInRange("Head Recentre Rate", &vehicle.camera.headLookRecenterRate, 0.0f, 30.0f, "%.1f", 0.1f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How quickly the head turns back to the road after looking round (0: it stays where it was turned).");
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Gamepad Feedback", ImGuiTreeNodeFlags_DefaultOpen))
    {
        VehicleHapticsSettings& haptics = vehicle.haptics;
        ImGui::Checkbox("Engine Rumble and Gear Thump", &haptics.enabled);
        ImGui::BeginDisabled(!haptics.enabled);
        DragFloatInRange("Rumble Strength", &haptics.rumbleStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        DragFloatInRange("Trigger Strength", &haptics.triggerStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        ImGui::Checkbox("Brake Trigger Shakes When a Wheel Locks", &haptics.brakeLockFeedback);
        ImGui::Checkbox("DualSense HD Haptics (USB)", &haptics.audioHaptics);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("A DualSense on a USB cable plays the engine, the road and the tyres on its actuators as waveforms, in place of the rumble.");
        }
        ImGui::BeginDisabled(!haptics.audioHaptics);
        DragFloatInRange("Engine Haptics", &haptics.engineStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        DragFloatInRange("Road Haptics", &haptics.roadStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        DragFloatInRange("Tyre Slip Haptics", &haptics.slipStrength, 0.0f, 2.0f, "%.2f", 0.01f);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::TextDisabled("DualSense: the triggers push back (RT accelerator, LT brake) and shake. Other pads: rumble only.");
    }

    if (ImGui::CollapsingHeader("Physics Overlay", ImGuiTreeNodeFlags_DefaultOpen))
    {
        VehiclePhysicsOverlaySettings& overlay = vehicle.overlay;
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
        DrawPhysicsRate(vehicle.physicsRateHz);
        if (status.active)
        {
            ImGui::TextDisabled("Other changes apply the next time driving starts.");
        }
        if (DrawTuning(vehicle.tuning) && status.active)
        {
            result.actions.brushTyreBristles = std::array<int, 2>{vehicle.tuning.brushTyreRibs, vehicle.tuning.brushTyreSegments};
        }
    }
}
}
