#include "suspension_rigs_panel.h"

#include <engine/asset/model_cache.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>
#include <engine/physics/vehicle_suspension.h>

#include <IconsPhosphor.h>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace me
{

namespace
{
using suspension::KinematicOutputs;
using suspension::Kinematics;
using suspension::Vec3;

constexpr const char* kModeNames[4] = {"Heave", "Pitch", "Roll", "Warp"};
constexpr const char* kWheelNames[4] = {"FL", "FR", "RL", "RR"};
// The data has no tyre width; the drawing uses this.
constexpr double kDrawnTyreWidth = 0.3;
constexpr double kDegrees = 180.0 / std::numbers::pi;

// Moves the linkage in small steps: the solver starts from where it is, and a slider dragged far in
// one frame would ask it to jump.
void SolveTo(Kinematics& kinematics, double travel, double rack)
{
    const double fromTravel = kinematics.Travel();
    const double fromRack = kinematics.Rack();
    const int steps = std::clamp(static_cast<int>(std::max(std::abs(travel - fromTravel) / 0.002, std::abs(rack - fromRack) / 0.001)) + 1, 1, 200);
    for (int i = 1; i <= steps; ++i)
    {
        const double t = static_cast<double>(i) / steps;
        kinematics.Solve(fromTravel + (travel - fromTravel) * t, fromRack + (rack - fromRack) * t);
    }
}

enum class View
{
    Rear, // from behind: the car's left on the left, up
    Side, // the left wheel from outside the car's left: forward to the right, up
    Top,  // from above: the car's left on the left, forward up
};

ImPlotPoint Project(View view, const Vec3& p)
{
    switch (view)
    {
    case View::Rear:
        return ImPlotPoint(-p.y * 1000.0, p.z * 1000.0);
    case View::Side:
        return ImPlotPoint(p.x * 1000.0, p.z * 1000.0);
    case View::Top:
    default:
        return ImPlotPoint(-p.y * 1000.0, p.x * 1000.0);
    }
}

// Line segments gathered for one ImPlot item (ImPlotLineFlags_Segments: pairs of points).
struct Segments
{
    std::vector<double> x;
    std::vector<double> y;

    void Add(View view, const Vec3& a, const Vec3& b)
    {
        const ImPlotPoint pa = Project(view, a);
        const ImPlotPoint pb = Project(view, b);
        x.push_back(pa.x);
        y.push_back(pa.y);
        x.push_back(pb.x);
        y.push_back(pb.y);
    }
    void Plot(const char* label, const ImVec4& colour, float weight) const
    {
        if (x.empty())
        {
            return;
        }
        ImPlotSpec spec;
        spec.LineColor = colour;
        spec.LineWeight = weight;
        spec.Flags = ImPlotLineFlags_Segments;
        ImPlot::PlotLine(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
    }
};

struct Markers
{
    std::vector<double> x;
    std::vector<double> y;

    void Add(View view, const Vec3& p)
    {
        const ImPlotPoint q = Project(view, p);
        x.push_back(q.x);
        y.push_back(q.y);
    }
    void Plot(const char* label, ImPlotMarker marker, const ImVec4& colour, float size) const
    {
        if (x.empty())
        {
            return;
        }
        ImPlotSpec spec;
        spec.Marker = marker;
        spec.MarkerSize = size;
        spec.MarkerFillColor = colour;
        spec.MarkerLineColor = colour;
        spec.LineWeight = 1.0f;
        ImPlot::PlotScatter(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
    }
};

// The game's data gives its springs as wheel rates; the seat points only exist for the motion ratio
// output, so the drawing leaves them out.
bool IsDrawn(const std::string& name)
{
    return name.rfind("spring_", 0) != 0;
}

// A tyre: its two sidewall circles and the tread's top and bottom between them, and its axle stub.
void DrawTyre(View view, const Vec3& center, const Vec3& spin, double radius, Segments& tyre, Segments& axis)
{
    const Vec3 e1 = glm::normalize(Vec3(0.0, 0.0, 1.0) - spin * spin.z);
    const Vec3 e2 = glm::cross(spin, e1);
    constexpr int kSides = 48;
    for (const double side : {-0.5, 0.5})
    {
        const Vec3 c = center + spin * (side * kDrawnTyreWidth);
        for (int i = 0; i < kSides; ++i)
        {
            const double a0 = 2.0 * std::numbers::pi * i / kSides;
            const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSides;
            tyre.Add(view, c + radius * (std::cos(a0) * e1 + std::sin(a0) * e2), c + radius * (std::cos(a1) * e1 + std::sin(a1) * e2));
        }
    }
    for (const double up : {-1.0, 1.0})
    {
        tyre.Add(view, center + up * radius * e1 - spin * (0.5 * kDrawnTyreWidth), center + up * radius * e1 + spin * (0.5 * kDrawnTyreWidth));
    }
    axis.Add(view, center, center + spin * (0.5 * kDrawnTyreWidth + 0.05));
}

// A solid axle where it is posed, in the axle's frame: its links from the chassis to the axle, the
// beam between the wheel centres, both tyres, and (rear view) the roll centre with the lines to it
// from the contact points.
void DrawSolidAxle(View view, const suspension::SolidAxle& axle, bool showGeometry, Segments& links, Segments& beam, Segments& tyre, Segments& axis,
                   Segments& road, Segments& geometry, Markers& chassis, Markers& joints, Markers& centres)
{
    const suspension::SolidAxleDefinition& def = axle.Definition();
    for (const suspension::AxleLinkDef& link : def.links)
    {
        const Vec3 end = axle.Point(link.axle);
        links.Add(view, link.chassis, end);
        chassis.Add(view, link.chassis);
        joints.Add(view, end);
    }
    const double half = 0.5 * def.track;
    beam.Add(view, axle.Point(Vec3(0.0, half, 0.0)), axle.Point(Vec3(0.0, -half, 0.0)));
    std::array<Vec3, 2> contacts{};
    for (int side = 0; side < 2; ++side)
    {
        if (view == View::Side && side == 1)
        {
            break;
        }
        KinematicOutputs out;
        axle.ComputeOutputs(side, out);
        const Vec3 origin(0.0, side == 0 ? half : -half, 0.0);
        DrawTyre(view, out.wheelCenter + origin, out.spinAxis, def.tyreRadius, tyre, axis);
        contacts[side] = out.contactPoint + origin;
        if (view == View::Side)
        {
            road.Add(view, contacts[side] - Vec3(0.6, 0.0, 0.0), contacts[side] + Vec3(0.6, 0.0, 0.0));
        }
        if (showGeometry && view == View::Rear && out.frontInstantCenterValid)
        {
            const Vec3 rollCentre = out.frontInstantCenter + origin;
            geometry.Add(view, contacts[side], rollCentre);
            centres.Add(view, rollCentre);
        }
    }
    if (view == View::Rear)
    {
        const Vec3 d = contacts[1] - contacts[0];
        road.Add(view, contacts[0] - d * 0.25, contacts[1] + d * 0.25);
    }
}

// One corner of the axle where its kinematics has it, in the axle's frame (x forward, y left, z up,
// from the axle's centre at the wheel centres' design height).
void DrawCorner(View view, const Kinematics& kinematics, double yOffset, bool showGeometry, Segments& links, Segments& knuckle, Segments& tyre,
                Segments& axis, Segments& road, Segments& geometry, Markers& chassis, Markers& joints, Markers& centres)
{
    const suspension::Model& model = kinematics.GetModel();
    const suspension::SuspensionDefinition& def = model.definition;
    const Vec3 offset(0.0, yOffset, 0.0);
    const auto at = [&](const std::string& name) {
        return kinematics.Point(model.Find(name)) + offset;
    };

    for (const suspension::BodyDef& body : def.bodies)
    {
        std::vector<std::string> names;
        for (const std::string& name : body.points)
        {
            if (IsDrawn(name) && model.Find(name) >= 0)
            {
                names.push_back(name);
            }
        }
        if (body.name == def.knuckle)
        {
            // The upright: from the wheel centre to each joint on it, and the joints to each other.
            for (const std::string& name : names)
            {
                if (name != def.wheelCenter)
                {
                    knuckle.Add(view, at(def.wheelCenter), at(name));
                }
            }
            continue;
        }
        if (names.size() == 2)
        {
            links.Add(view, at(names[0]), at(names[1]));
        }
        else
        {
            for (size_t i = 0; i < names.size(); ++i)
            {
                links.Add(view, at(names[i]), at(names[(i + 1) % names.size()]));
            }
        }
    }
    for (const suspension::SliderDef& slider : def.sliders)
    {
        links.Add(view, at(slider.through), at(slider.base));
    }
    for (size_t i = 0; i < model.pointNames.size(); ++i)
    {
        if (!IsDrawn(model.pointNames[i]) || model.pointNames[i] == def.wheelCenter || model.pointNames[i].find("axis") != std::string::npos)
        {
            continue;
        }
        const Vec3 p = kinematics.Point(static_cast<int>(i)) + offset;
        (model.roles[i] == suspension::PointRole::Moving ? joints : chassis).Add(view, p);
    }

    KinematicOutputs out;
    suspension::ComputeOutputs(kinematics, out);
    const Vec3 center = out.wheelCenter + offset;
    const double radius = def.tyreRadius;
    DrawTyre(view, center, out.spinAxis, radius, tyre, axis);

    const Vec3 contact = out.contactPoint + offset;
    if (view == View::Side)
    {
        road.Add(view, contact - Vec3(0.6, 0.0, 0.0), contact + Vec3(0.6, 0.0, 0.0));
    }
    if (!showGeometry)
    {
        return;
    }
    if (out.kingpinValid)
    {
        // The steering axis from the road up past the upper joint.
        const Vec3 k = out.kingpinPoint + offset;
        const Vec3 dir = out.kingpinAxis;
        if (std::abs(dir.z) > 1e-3)
        {
            const double down = (contact.z - k.z) / dir.z;
            const double up = (center.z + radius * 0.9 - k.z) / dir.z;
            geometry.Add(view, k + dir * down, k + dir * up);
        }
    }
    if (view == View::Rear && out.frontInstantCenterValid)
    {
        // Contact point through the front-view instant centre to the car's centre plane: the roll centre.
        const Vec3 ic = out.frontInstantCenter + offset;
        centres.Add(view, ic);
        const Vec3 d = ic - contact;
        if (std::abs(d.y) > 1e-6)
        {
            const double s = -contact.y / d.y;
            const Vec3 rollCentre = contact + d * s;
            geometry.Add(view, contact, std::abs(s) > 1.0 ? rollCentre : ic);
            if (std::abs(s) <= 1.0)
            {
                geometry.Add(view, ic, rollCentre);
            }
            centres.Add(view, rollCentre);
        }
    }
    if (view == View::Side && out.sideInstantCenterValid)
    {
        // The side-view instant centre (anti-dive, anti-squat); a far one only shows its direction.
        const Vec3 ic = out.sideInstantCenter + offset;
        const Vec3 d = ic - contact;
        const double length = glm::length(d);
        if (length > 1e-6)
        {
            geometry.Add(view, contact, length > 3.0 ? contact + d * (1.5 / length) : ic);
            if (length <= 3.0)
            {
                centres.Add(view, ic);
            }
        }
    }
}

void DrawLinkageView(const char* title, View view, const std::array<std::unique_ptr<Kinematics>, 2>& corners, const suspension::SolidAxle* solid,
                     const std::array<double, 2>& yOffsets, bool showGeometry, bool fit, const ImVec2& size)
{
    if (fit)
    {
        ImPlot::SetNextAxesToFit();
    }
    if (!ImPlot::BeginPlot(title, size, ImPlotFlags_Equal | ImPlotFlags_NoMenus))
    {
        return;
    }
    const char* xLabel = view == View::Side ? "forward (mm)" : "left <-   (mm)   -> right";
    const char* yLabel = view == View::Top ? "forward (mm)" : "up (mm)";
    ImPlot::SetupAxes(xLabel, yLabel);
    ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_None);

    Segments links, knuckle, tyre, axis, road, geometry;
    Markers chassis, joints, centres;
    for (int side = 0; side < 2 && solid == nullptr; ++side)
    {
        if (view == View::Side && side == 1)
        {
            break; // the right wheel would sit on top of the left
        }
        DrawCorner(view, *corners[side], yOffsets[side], showGeometry, links, knuckle, tyre, axis, road, geometry, chassis, joints, centres);
    }
    if (solid != nullptr)
    {
        DrawSolidAxle(view, *solid, showGeometry, links, knuckle, tyre, axis, road, geometry, chassis, joints, centres);
    }
    else if (view == View::Rear)
    {
        // The road under both tyres (pads at the contact points).
        KinematicOutputs left;
        KinematicOutputs right;
        suspension::ComputeOutputs(*corners[0], left);
        suspension::ComputeOutputs(*corners[1], right);
        const Vec3 a = left.contactPoint + Vec3(0.0, yOffsets[0], 0.0);
        const Vec3 b = right.contactPoint + Vec3(0.0, yOffsets[1], 0.0);
        const Vec3 d = b - a;
        road.Add(view, a - d * 0.25, b + d * 0.25);
    }
    road.Plot("Road", ImVec4(0.55f, 0.55f, 0.55f, 1.0f), 2.0f);
    tyre.Plot("Tyre", ImVec4(0.45f, 0.45f, 0.5f, 1.0f), 1.0f);
    axis.Plot("Tyre", ImVec4(0.45f, 0.45f, 0.5f, 1.0f), 1.0f);
    links.Plot("Links", ImVec4(0.35f, 0.65f, 1.0f, 1.0f), 2.5f);
    knuckle.Plot(solid != nullptr ? "Axle" : "Upright", ImVec4(1.0f, 0.6f, 0.2f, 1.0f), solid != nullptr ? 4.0f : 2.5f);
    chassis.Plot("Chassis pivots", ImPlotMarker_Square, ImVec4(0.8f, 0.8f, 0.8f, 1.0f), 4.0f);
    joints.Plot("Joints", ImPlotMarker_Circle, ImVec4(1.0f, 0.85f, 0.3f, 1.0f), 3.5f);
    if (showGeometry)
    {
        geometry.Plot("Geometry", ImVec4(0.6f, 0.9f, 0.5f, 0.8f), 1.0f);
        centres.Plot("Geometry", ImPlotMarker_Cross, ImVec4(0.6f, 0.9f, 0.5f, 1.0f), 6.0f);
    }
    ImPlot::EndPlot();
}

// A vertical marker in the current plot.
void MarkX(const char* label, double x)
{
    ImPlotSpec spec;
    spec.LineColor = ImVec4(1.0f, 1.0f, 1.0f, 0.35f);
    spec.LineWeight = 1.0f;
    ImPlot::PlotInfLines(label, &x, 1, spec);
}

// Data read from an array of structs: consecutive values are stride bytes apart.
ImPlotSpec Strided(size_t stride)
{
    ImPlotSpec spec;
    spec.Stride = static_cast<int>(stride);
    return spec;
}

void TableRow(const char* name, double front, double rear, const char* format = "%.2f")
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    ImGui::Text(format, front);
    ImGui::TableNextColumn();
    ImGui::Text(format, rear);
}

std::string Hz(double value, const char* none)
{
    if (value <= 0.0)
    {
        return none;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}
}

SuspensionRigsPanel::~SuspensionRigsPanel()
{
    if (m_run)
    {
        // Stops between tests: at most one sweep's worth of waiting.
        m_run->cancel = true;
        if (m_run->result.valid())
        {
            m_run->result.wait();
        }
    }
}

SuspensionRigsPanel::SuspensionRigsPanel()
    : EditorPanel("suspension_rigs", "Suspension Rigs", ICON_PH_CHART_LINE)
{
}

void SuspensionRigsPanel::Tick(EditorContext& context)
{
    // Also while the window is not drawn (fullscreen): the running rig keeps its settings. Until
    // the window has been shown the rig keeps what it has.
    if (m_shown)
    {
        context.result.vehicleRigExcitation = m_excitation;
    }
    if (!IsOpen() && context.state.vehicleRigStatus.active)
    {
        // Closing the window takes the car off the rig.
        context.result.actions.stopVehicleRig = true;
    }
}

void SuspensionRigsPanel::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 760.0f), ImGuiCond_FirstUseEver);
}

void SuspensionRigsPanel::OnGui(EditorContext& context)
{
    DrawContents(context.scene, context.state.vehicleRigStatus, context.result);
}

void SuspensionRigsPanel::DrawContents(const IEditorWorld& scene, const VehicleRigStatus& live, EditorUiFrameResult& result)
{
    m_shown = true;
    result.vehicleRigExcitation = m_excitation;
    PollRun();
    DrawCarSource(scene);
    if (!m_car.has_value())
    {
        return;
    }
    DrawRunControls();
    ImGui::Separator();

    if (ImGui::BeginTabBar("SuspensionRigTabs"))
    {
        const auto flags = [&](Tab tab) {
            return m_requestedTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem(ICON_PH_PLAY " Live Rig", nullptr, flags(LiveTab)))
        {
            DrawLiveTab(scene, live, result);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_PH_GEAR_FINE " Linkage", nullptr, flags(LinkageTab)))
        {
            DrawLinkageTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_PH_TABLE " Summary", nullptr, flags(SummaryTab)))
        {
            DrawSummaryTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_PH_CHART_LINE " K&C", nullptr, flags(KcTab)))
        {
            DrawKcTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_PH_WAVE_SQUARE " Seven-Post", nullptr, flags(SevenPostTab)))
        {
            DrawSevenPostTab();
            ImGui::EndTabItem();
        }
        m_requestedTab = -1;
        ImGui::EndTabBar();
    }
}

void SuspensionRigsPanel::DrawCarSource(const IEditorWorld& scene)
{
    std::string selectedPath;
    std::string selectedName;
    if (scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity()))
    {
        selectedPath = scene.GetModel(scene.GetSelectedEntity()).sourcePath;
        selectedName = scene.GetTag(scene.GetSelectedEntity()).name;
    }
    // The first car selected while the window is open loads by itself.
    if (!m_car.has_value() && m_carError.empty() && !selectedPath.empty() && selectedPath != m_sourcePath)
    {
        LoadCar(selectedPath, selectedName);
    }

    if (m_car.has_value())
    {
        ImGui::Text(ICON_PH_CAR " %s", m_carName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f kg, wheelbase %.2f m, rack %.1f mm at full lock", m_car->mass, m_car->wheelbase, m_car->rackAtLock * 1000.0);
    }
    else
    {
        ImGui::TextDisabled("Select a car whose model carries its suspension data (imported from Assetto Corsa).");
    }
    const bool otherSelected = !selectedPath.empty() && selectedPath != m_sourcePath;
    ImGui::BeginDisabled(!otherSelected || m_run != nullptr);
    if (ImGui::Button(ICON_PH_CURSOR " Use Selected Model"))
    {
        LoadCar(selectedPath, selectedName);
    }
    ImGui::EndDisabled();
    if (!m_carError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ui_colors::kTextDanger, "%s", m_carError.c_str());
    }
}

void SuspensionRigsPanel::LoadCar(const std::string& sourcePath, const std::string& name)
{
    m_sourcePath = sourcePath;
    m_carName = name.empty() ? sourcePath : name;
    m_car.reset();
    m_report.reset();
    m_runError.clear();
    m_carError.clear();
    m_kinematics = {};
    m_solid.reset();
    m_kinematicsAxle = -1;
    const std::shared_ptr<const LoadedModelData> model = ModelCache::Get(sourcePath);
    if (!model)
    {
        m_carError = "the model's data is not loaded";
        return;
    }
    if (!model->carSpec.has_value() || !model->carSpec->frontSuspension.has_value() || !model->carSpec->rearSuspension.has_value())
    {
        m_carError = "this model carries no suspension linkage";
        return;
    }
    try
    {
        m_car = BuildCarModel(*model->carSpec, m_carName);
    }
    catch (const std::exception& error)
    {
        m_carError = error.what();
    }
}

void SuspensionRigsPanel::DrawRunControls()
{
    if (m_run)
    {
        std::string stage;
        {
            const std::lock_guard lock(m_run->mutex);
            stage = m_run->stage;
        }
        ImGui::ProgressBar(static_cast<float>(m_run->progress.load()), ImVec2(-120.0f, 0.0f), stage.c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_X " Cancel", ImVec2(-1.0f, 0.0f)))
        {
            m_run->cancel = true;
        }
        return;
    }
    if (ImGui::Button(ICON_PH_PLAY " Run Rigs"))
    {
        StartRun();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Compare with an assumed damper friction", &m_options.frictionComparison);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Runs the seven-post tests again with a seal friction at each wheel (Coulomb 60 N, breakaway 90 N).\n"
                          "The data has none: this only shows what one would do. Doubles the run time.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    float amplitude = static_cast<float>(m_options.sweepAmplitude * 1000.0);
    if (ImGui::DragFloat("Sweep (mm)", &amplitude, 0.05f, 0.5f, 20.0f, "%.1f"))
    {
        m_options.sweepAmplitude = amplitude / 1000.0;
    }
    if (m_report.has_value())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("last run %.1f s", m_report->seconds);
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_PH_COPY " Copy Summary"))
        {
            ImGui::SetClipboardText(suspension::FormatRigSummary(*m_report).c_str());
        }
    }
    if (!m_runError.empty())
    {
        ImGui::TextColored(ui_colors::kTextDanger, "%s", m_runError.c_str());
    }
}

void SuspensionRigsPanel::StartRun()
{
    if (m_run || !m_car.has_value())
    {
        return;
    }
    auto run = std::make_unique<Run>();
    Run* state = run.get();
    const suspension::CarModel car = *m_car;
    const suspension::RigReportOptions options = m_options;
    state->result = RunAsync(TaskPriority::Low, [state, car, options] {
        return suspension::RunRigReport(car, options, [state](double done, const char* stage) {
            state->progress = done;
            const std::lock_guard lock(state->mutex);
            state->stage = stage;
            return !state->cancel.load();
        });
    });
    m_run = std::move(run);
    m_runError.clear();
}

void SuspensionRigsPanel::PollRun()
{
    if (!m_run || m_run->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }
    try
    {
        std::optional<suspension::RigReport> report = m_run->result.get();
        if (report.has_value())
        {
            m_report = std::move(report);
        }
    }
    catch (const std::exception& error)
    {
        m_runError = std::string("the rigs failed: ") + error.what();
    }
    m_run.reset();
}

void SuspensionRigsPanel::DrawSummaryTab()
{
    if (!m_report.has_value())
    {
        ImGui::TextDisabled("Run the rigs to see the car's K&C and seven-post figures.");
        return;
    }
    const suspension::RigReport& r = *m_report;
    const suspension::KcResult& kc = r.kc;
    ImGui::Text("Mass %.0f kg (sprung %.0f), wheelbase %.3f m, centre of mass %.3f m up; inertia roll %.0f, pitch %.0f kg m^2",
                r.mass, r.sprungMass, r.wheelbase, r.cgHeight, r.rollInertia, r.pitchInertia);
    ImGui::Spacing();
    if (ImGui::BeginTable("kc", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("K&C");
        ImGui::TableSetupColumn("front");
        ImGui::TableSetupColumn("rear");
        ImGui::TableHeadersRow();
        const suspension::KcAxleSummary& f = kc.axles[0];
        const suspension::KcAxleSummary& b = kc.axles[1];
        TableRow("wheel rate (N/mm)", f.wheelRate, b.wheelRate);
        TableRow("bump steer (deg/m, toe-in +)", f.bumpSteer, b.bumpSteer);
        TableRow("camber gain (deg/m, to body)", f.camberGain, b.camberGain);
        TableRow("half-track change (mm/m)", f.trackChange, b.trackChange);
        TableRow("roll centre height (mm)", f.rollCenterHeight, b.rollCenterHeight);
        TableRow("roll stiffness (Nm/deg)", f.rollStiffness, b.rollStiffness, "%.0f");
        TableRow("roll steer (deg/deg, outward +)", f.rollSteer, b.rollSteer);
        TableRow("roll camber, outer to road (deg/deg)", f.rollCamber, b.rollCamber);
        TableRow("kingpin inclination (deg)", f.kingpinInclination, b.kingpinInclination);
        TableRow("caster (deg)", f.caster, b.caster);
        TableRow("scrub radius (mm)", f.scrubRadius, b.scrubRadius, "%.1f");
        TableRow("caster trail (mm)", f.casterTrail, b.casterTrail, "%.1f");
        TableRow("contact path angle (deg)", f.contactPathAngle, b.contactPathAngle);
        TableRow("wheel centre path angle (deg)", f.centerPathAngle, b.centerPathAngle);
        ImGui::EndTable();
    }
    ImGui::Text("Roll stiffness on the front %.1f %%, steering ratio %.2f:1", kc.rollStiffnessFrontShare * 100.0, kc.steeringRatio);
    ImGui::Text("Anti-dive front %.0f %%, anti-lift rear %.0f %%, anti-squat rear %.0f %%", kc.antiDiveFront, kc.antiLiftRear, kc.antiSquatRear);
    if (!kc.steer.empty())
    {
        ImGui::Text("Ackermann at full lock %.0f %% (left %.1f deg, right %.1f deg)", kc.steer.back().ackermann, kc.steer.back().left, kc.steer.back().right);
    }

    ImGui::Spacing();
    const int columns = r.hasFriction ? 8 : 6;
    if (ImGui::BeginTable("sevenpost", columns, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("seven-post");
        ImGui::TableSetupColumn("body f (Hz)");
        ImGui::TableSetupColumn("peak gain");
        ImGui::TableSetupColumn("damping");
        ImGui::TableSetupColumn("wheel hop (Hz)");
        ImGui::TableSetupColumn("peak load variation");
        if (r.hasFriction)
        {
            ImGui::TableSetupColumn("friction: body f");
            ImGui::TableSetupColumn("friction: load var.");
        }
        ImGui::TableHeadersRow();
        for (int m = 0; m < 4; ++m)
        {
            const suspension::SweepResult& s = r.sweeps[m];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kModeNames[m]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Hz(s.bodyFrequency, "-").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Hz(s.bodyPeakGain, "-").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Hz(s.bodyDamping, "-").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Hz(s.wheelHopFrequency, "none (over-damped)").c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", s.peakLoadVariation);
            if (r.hasFriction)
            {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Hz(r.frictionSweeps[m].bodyFrequency, "-").c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.frictionSweeps[m].peakLoadVariation);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Text("Heave step %.0f mm: overshoot %.0f %%, damping ratio %.2f, settles in %.2f s", r.options.stepHeight * 1000.0, r.step.overshoot * 100.0,
                r.step.dampingRatio, r.step.settlingTime);
    if (!r.aero.empty())
    {
        ImGui::Text("Aero %.0f N (%.0f %% front): front down %.1f mm, rear down %.1f mm", r.aero.back().downforce, r.options.aeroFrontShare * 100.0,
                    -r.aero.back().frontHeight, -r.aero.back().rearHeight);
    }
    ImGui::Text("Warp %.0f mm: diagonal load transfer %.0f N (%.1f N/mm)", r.options.warp * 1000.0, r.warp.diagonalTransfer, r.warp.warpStiffness);
    for (const suspension::RigRoadCase& road : r.roads)
    {
        ImGui::Text("Road '%s' at %.0f m/s: load RMS FL %.3f, RL %.3f of static; body %.2f m/s^2 RMS; lift-off %.2f s", road.name.c_str(), r.options.roadSpeed,
                    road.plain.loadRms[0], road.plain.loadRms[2], road.plain.bodyAccelRms, road.plain.liftOffSeconds);
    }
}

void SuspensionRigsPanel::DrawKcTab()
{
    if (!m_report.has_value())
    {
        ImGui::TextDisabled("Run the rigs to plot the K&C sweeps.");
        return;
    }
    const suspension::KcResult& kc = m_report->kc;
    const char* axleNames[2] = {"front", "rear"};
    const ImVec2 size(-1.0f, std::max(ImGui::GetContentRegionAvail().y, 400.0f));
    if (!ImPlot::BeginSubplots("##kc", 2, 4, size, ImPlotSubplotFlags_None))
    {
        return;
    }
    // The linkage tab's pose, marked on the plots of its axle's sweeps.
    const auto bounce = [&](const char* title, const char* yLabel, auto field) {
        if (ImPlot::BeginPlot(title))
        {
            ImPlot::SetupAxes("travel (mm, bump +)", yLabel, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            for (int axle = 0; axle < 2; ++axle)
            {
                const auto& points = kc.bounce[axle];
                if (!points.empty())
                {
                    ImPlot::PlotLine(axleNames[axle], &points[0].travel, &field(points[0]), static_cast<int>(points.size()), Strided(sizeof(points[0])));
                }
            }
            MarkX("linkage", m_heaveMm);
            ImPlot::EndPlot();
        }
    };
    bounce("Camber (left wheel, to body)", "deg, top out +", [](const suspension::KcBouncePoint& p) -> const double& { return p.left.camber; });
    bounce("Toe: bump steer (left wheel)", "deg, toe-in +", [](const suspension::KcBouncePoint& p) -> const double& { return p.left.toe; });
    bounce("Half-track change", "mm, out +", [](const suspension::KcBouncePoint& p) -> const double& { return p.left.halfTrackChange; });
    bounce("Roll centre height", "mm above road", [](const suspension::KcBouncePoint& p) -> const double& { return p.rollCenterHeight; });

    const auto roll = [&](const char* title, const char* yLabel, auto field) {
        if (ImPlot::BeginPlot(title))
        {
            ImPlot::SetupAxes("body roll (deg, right side down)", yLabel, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            for (int axle = 0; axle < 2; ++axle)
            {
                const auto& points = kc.roll[axle];
                if (!points.empty())
                {
                    ImPlot::PlotLine(axleNames[axle], &points[0].roll, &field(points[0]), static_cast<int>(points.size()), Strided(sizeof(points[0])));
                }
            }
            MarkX("linkage", m_rollDegrees);
            ImPlot::EndPlot();
        }
    };
    roll("Roll moment (springs + anti-roll bar)", "Nm", [](const suspension::KcRollPoint& p) -> const double& { return p.rollMoment; });
    roll("Outer wheel camber to road", "deg", [](const suspension::KcRollPoint& p) -> const double& { return p.right.camber; });

    if (ImPlot::BeginPlot("Steering: wheel angles"))
    {
        ImPlot::SetupAxes("steering wheel (deg)", "wheel turned right (deg)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        if (!kc.steer.empty())
        {
            const int n = static_cast<int>(kc.steer.size());
            ImPlot::PlotLine("left", &kc.steer[0].steeringWheel, &kc.steer[0].left, n, Strided(sizeof(suspension::KcSteerPoint)));
            ImPlot::PlotLine("right", &kc.steer[0].steeringWheel, &kc.steer[0].right, n, Strided(sizeof(suspension::KcSteerPoint)));
            if (m_car.has_value())
            {
                MarkX("linkage", m_steering * m_car->steeringWheelLockDegrees);
            }
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Ackermann"))
    {
        ImPlot::SetupAxes("steering wheel (deg)", "% of ideal", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        // Undefined while the wheels have hardly turned: a gap there.
        std::vector<double> wheel;
        std::vector<double> ackermann;
        for (const suspension::KcSteerPoint& p : kc.steer)
        {
            wheel.push_back(p.steeringWheel);
            ackermann.push_back(std::max(std::abs(p.left), std::abs(p.right)) > 1.0 ? p.ackermann : std::nan(""));
        }
        ImPlot::PlotLine("Ackermann", wheel.data(), ackermann.data(), static_cast<int>(wheel.size()));
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
}

void SuspensionRigsPanel::DrawSevenPostTab()
{
    if (!m_report.has_value())
    {
        ImGui::TextDisabled("Run the rigs to plot the seven-post sweeps.");
        return;
    }
    const suspension::RigReport& r = *m_report;
    for (int m = 0; m < 4; ++m)
    {
        if (m > 0)
        {
            ImGui::SameLine();
        }
        ImGui::RadioButton(kModeNames[m], &m_mode, m);
    }
    if (r.hasFriction)
    {
        ImGui::SameLine();
        ImGui::Checkbox("Overlay the friction run", &m_showFriction);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("pads %.1f mm, at most %.2f m/s", r.options.sweepAmplitude * 1000.0, r.options.sweepMaxVelocity);

    const suspension::SweepResult& plain = r.sweeps[m_mode];
    const suspension::SweepResult* rough = r.hasFriction && m_showFriction ? &r.frictionSweeps[m_mode] : nullptr;
    const auto sweepLine = [](const char* label, const suspension::SweepResult& s, const double* firstY) {
        if (!s.cycles.empty())
        {
            ImPlot::PlotLine(label, &s.cycles[0].frequency, firstY, static_cast<int>(s.cycles.size()), Strided(sizeof(suspension::SweepCycle)));
        }
    };
    const auto frequencyAxis = [](const char* yLabel) {
        ImPlot::SetupAxes("frequency (Hz)", yLabel, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
    };

    const ImVec2 size(-1.0f, std::max(ImGui::GetContentRegionAvail().y, 420.0f));
    if (!ImPlot::BeginSubplots("##sevenpost", 2, 3, size, ImPlotSubplotFlags_None))
    {
        return;
    }
    if (ImPlot::BeginPlot("Body gain"))
    {
        frequencyAxis("body / pad");
        if (m_mode != 3)
        {
            sweepLine("body", plain, plain.cycles.empty() ? nullptr : &plain.cycles[0].bodyGain);
            if (rough)
            {
                sweepLine("body (friction)", *rough, rough->cycles.empty() ? nullptr : &rough->cycles[0].bodyGain);
            }
        }
        if (plain.bodyFrequency > 0.0)
        {
            MarkX("body f", plain.bodyFrequency);
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Body phase"))
    {
        frequencyAxis("deg");
        if (m_mode != 3)
        {
            sweepLine("phase", plain, plain.cycles.empty() ? nullptr : &plain.cycles[0].bodyPhase);
            if (rough)
            {
                sweepLine("phase (friction)", *rough, rough->cycles.empty() ? nullptr : &rough->cycles[0].bodyPhase);
            }
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Body acceleration gain"))
    {
        frequencyAxis("body / pad acceleration");
        if (m_mode != 3)
        {
            sweepLine("accel", plain, plain.cycles.empty() ? nullptr : &plain.cycles[0].bodyAccelGain);
            if (rough)
            {
                sweepLine("accel (friction)", *rough, rough->cycles.empty() ? nullptr : &rough->cycles[0].bodyAccelGain);
            }
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Tyre load variation"))
    {
        frequencyAxis("load amplitude / static");
        for (int w = 0; w < 4; ++w)
        {
            sweepLine(kWheelNames[w], plain, plain.cycles.empty() ? nullptr : &plain.cycles[0].loadVariation[w]);
        }
        if (rough)
        {
            sweepLine("FL (friction)", *rough, rough->cycles.empty() ? nullptr : &rough->cycles[0].loadVariation[0]);
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Wheel gain"))
    {
        frequencyAxis("hub / pad");
        for (int w = 0; w < 4; ++w)
        {
            sweepLine(kWheelNames[w], plain, plain.cycles.empty() ? nullptr : &plain.cycles[0].wheelGain[w]);
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Heave step"))
    {
        const suspension::StepResponse& step = r.step;
        ImPlot::SetupAxes("time (s)", "body (mm)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxis(ImAxis_Y2, "FL tyre load (N)", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
        if (!step.time.empty())
        {
            std::vector<double> body(step.body.size());
            std::transform(step.body.begin(), step.body.end(), body.begin(), [](double v) { return v * 1000.0; });
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::PlotLine("body", step.time.data(), body.data(), static_cast<int>(body.size()));
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::PlotLine("FL load", step.time.data(), step.frontLoad.data(), static_cast<int>(step.frontLoad.size()));
        }
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
}

void SuspensionRigsPanel::DrawLiveTab(const IEditorWorld& scene, const VehicleRigStatus& live, EditorUiFrameResult& result)
{
    VehicleRigExcitation& e = m_excitation;
    if (live.active)
    {
        if (ImGui::Button(ICON_PH_STOP " Stop the Rig"))
        {
            result.actions.stopVehicleRig = true;
        }
        ImGui::SameLine();
        ImGui::Text("'%s' on the rig: %.2f s simulated", live.vehicleName.c_str(), live.time);
        if (live.inputFrequency > 0.0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("input %.2f Hz", live.inputFrequency);
        }
    }
    else
    {
        const bool canStart = scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity());
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button(ICON_PH_PLAY " Put the Selected Car on the Rig"))
        {
            result.actions.startVehicleRig = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("The car, its wheels and the scene's pads (\"Wheel pad FL\"...) and loaders (\"Aero loader ...\") move in the viewport.");
        if (!live.lastError.empty())
        {
            ImGui::TextColored(ui_colors::kTextDanger, "%s", live.lastError.c_str());
        }
    }

    int waveform = static_cast<int>(e.waveform);
    ImGui::TextUnformatted("Input:");
    for (const auto& [label, value] :
         {std::pair<const char*, int>{"Sine", 0}, {"Sweep 0.5-20 Hz", 1}, {"Step", 2}, {"Random road", 3}, {"Body loads", 4}})
    {
        ImGui::SameLine();
        ImGui::RadioButton(label, &waveform, value);
        if (value == 4 && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The pads stay level; the three body loaders push the body (heave), pitch it (as braking does)\n"
                              "or roll it (as cornering does), so it moves on its springs. The pad inputs instead move the\n"
                              "ground under the car, which below the body's resonance mostly tilts the car as a whole.");
        }
    }
    e.waveform = static_cast<VehicleRigWaveform>(waveform);

    int mode = static_cast<int>(e.mode);
    ImGui::BeginDisabled(e.waveform == VehicleRigWaveform::Road);
    ImGui::TextUnformatted("Mode: ");
    for (int m = 0; m < 4; ++m)
    {
        ImGui::SameLine();
        ImGui::RadioButton(kModeNames[m], &mode, m);
    }
    ImGui::EndDisabled();
    e.mode = static_cast<suspension::RigMode>(mode);

    ImGui::SetNextItemWidth(200.0f);
    if (e.waveform == VehicleRigWaveform::Road)
    {
        ImGui::SliderFloat("Speed (m/s)", &e.roadSpeed, 5.0f, 80.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Road", &m_roadRoughness, "Smooth track\0Bumpy road\0Rough road\0");
        constexpr float kRoughness[3] = {1e-7f, 2e-6f, 1e-5f};
        e.roadRoughness = kRoughness[std::clamp(m_roadRoughness, 0, 2)];
    }
    else if (e.waveform == VehicleRigWaveform::BodyLoads)
    {
        ImGui::SliderFloat("Body load (g)", &e.bodyLoad, 0.05f, 2.0f, "%.2f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Heave: the sprung weight times this. Pitch and roll: that times the centre of mass's height,\n"
                              "the moment of braking or cornering at this many g.");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Frequency (Hz)", &e.frequency, 0.2f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    }
    else
    {
        float amplitude = e.amplitude * 1000.0f;
        if (ImGui::SliderFloat("Amplitude (mm)", &amplitude, 1.0f, 50.0f, "%.1f"))
        {
            e.amplitude = amplitude / 1000.0f;
        }
        if (e.waveform != VehicleRigWaveform::Sweep)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat(e.waveform == VehicleRigWaveform::Step ? "Steps per second / 2" : "Frequency (Hz)", &e.frequency, 0.2f, 20.0f, "%.2f",
                               ImGuiSliderFlags_Logarithmic);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(pads at most 0.5 m/s: high frequencies move less)");
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Slow motion (sim s per s)", &e.playbackRate, 0.02f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Drawn motion x", &e.exaggeration, 1.0f, 20.0f, "%.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Pads, body and wheels are drawn this many times further than they move. The plots are true size.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Damper friction (assumed)", &e.friction);
    ImGui::SameLine();
    ImGui::Checkbox("Linkage on the car", &m_showLinkage);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Draws the suspension at each wheel in the viewport, over the body, moving with the drawn motion.");
    }
    result.vehicleRigExcitation = e;

    if (live.sampleTime.empty())
    {
        ImGui::TextDisabled("Put the car on the rig to see the pads, the body and the tyres move.");
        return;
    }
    const int n = static_cast<int>(live.sampleTime.size());
    const double t1 = live.sampleTime.back();
    const double t0 = t1 - 10.0;
    const auto scaled = [](const std::vector<double>& v, double k) {
        std::vector<double> out(v.size());
        std::transform(v.begin(), v.end(), out.begin(), [k](double x) { return x * k; });
        return out;
    };
    const float height = std::max((ImGui::GetContentRegionAvail().y - 8.0f) / 3.0f, 160.0f);
    if (ImPlot::BeginPlot("Pads and body", ImVec2(-1.0f, height)))
    {
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        ImPlot::SetupAxes("simulated time (s)", "mm", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxis(ImAxis_Y2, "deg", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
        for (int i = 0; i < 4; ++i)
        {
            const std::vector<double> pad = scaled(live.pad[i], 1000.0);
            ImPlot::PlotLine((std::string("pad ") + kWheelNames[i]).c_str(), live.sampleTime.data(), pad.data(), n);
        }
        const std::vector<double> heave = scaled(live.heave, 1000.0);
        ImPlot::PlotLine("body heave", live.sampleTime.data(), heave.data(), n);
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        ImPlot::PlotLine("pitch (nose up)", live.sampleTime.data(), live.pitch.data(), n);
        ImPlot::PlotLine("roll (right down)", live.sampleTime.data(), live.roll.data(), n);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Tyre loads", ImVec2(-1.0f, height)))
    {
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        ImPlot::SetupAxes("simulated time (s)", "N", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
        for (int i = 0; i < 4; ++i)
        {
            ImPlot::PlotLine(kWheelNames[i], live.sampleTime.data(), live.tyreLoad[i].data(), n);
        }
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Suspension travel", ImVec2(-1.0f, height)))
    {
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        ImPlot::SetupAxes("simulated time (s)", "mm, bump +", ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, t0, t1, ImPlotCond_Always);
        for (int i = 0; i < 4; ++i)
        {
            const std::vector<double> travel = scaled(live.travel[i], 1000.0);
            ImPlot::PlotLine(kWheelNames[i], live.sampleTime.data(), travel.data(), n);
        }
        ImPlot::EndPlot();
    }
}

void SuspensionRigsPanel::PoseLinkage()
{
    const suspension::CarModel& car = *m_car;
    if (m_kinematicsAxle != m_axle)
    {
        m_kinematics = {};
        m_solid.reset();
        if (car.solidAxles[m_axle].has_value())
        {
            m_solid = std::make_unique<suspension::SolidAxle>(*car.solidAxles[m_axle], suspension::MakeCornerUnit(car.corners[m_axle * 2]),
                                                              suspension::MakeCornerUnit(car.corners[m_axle * 2 + 1]));
        }
        else
        {
            for (int side = 0; side < 2; ++side)
            {
                m_kinematics[side] = std::make_unique<Kinematics>(suspension::Compile(car.corners[m_axle * 2 + side].definition));
            }
        }
        m_kinematicsAxle = m_axle;
    }
    if (m_animation != 0)
    {
        m_animationTime += ImGui::GetIO().DeltaTime;
        const float wave = static_cast<float>(std::sin(2.0 * std::numbers::pi * m_animationHz * m_animationTime));
        switch (m_animation)
        {
        case 1:
            m_heaveMm = 45.0f * wave;
            break;
        case 2:
            m_rollDegrees = 3.0f * wave;
            break;
        case 3:
            m_steering = wave;
            break;
        default:
            break;
        }
    }
    const double roll = m_rollDegrees / kDegrees;
    const double rack = m_axle == 0 ? car.rackAtLock * m_steering : 0.0;
    m_poseFailed = false;
    std::array<double, 2> travel{};
    for (int side = 0; side < 2; ++side)
    {
        // The K&C rig's roll: the body turns, the pads stay level, z = -y sin(roll).
        const double y = car.corners[m_axle * 2 + side].position.y;
        travel[side] = m_heaveMm / 1000.0 - y * std::sin(roll);
    }
    if (m_solid)
    {
        m_solid->Solve(travel[0], travel[1]);
        return;
    }
    for (int side = 0; side < 2; ++side)
    {
        SolveTo(*m_kinematics[side], travel[side], rack);
        m_poseFailed = m_poseFailed || m_kinematics[side]->LastReport().status != suspension::SolveStatus::Converged;
    }
}

void SuspensionRigsPanel::DrawLinkageTab()
{
    ImGui::SetNextItemWidth(120.0f);
    const int previousAxle = m_axle;
    ImGui::Combo("Axle", &m_axle, "Front\0Rear\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Travel (mm)", &m_heaveMm, -60.0f, 60.0f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Roll (deg)", &m_rollDegrees, -4.0f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_axle != 0);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Steering", &m_steering, -1.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Centre"))
    {
        m_heaveMm = 0.0f;
        m_rollDegrees = 0.0f;
        m_steering = 0.0f;
        m_animation = 0;
    }

    ImGui::TextUnformatted("Animate:");
    ImGui::SameLine();
    ImGui::RadioButton("Off", &m_animation, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Bounce", &m_animation, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Roll", &m_animation, 2);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_axle != 0);
    ImGui::RadioButton("Steer", &m_animation, 3);
    ImGui::EndDisabled();
    if (m_axle != 0 && m_animation == 3)
    {
        m_animation = 0;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("Hz", &m_animationHz, 0.1f, 3.0f, "%.1f");
    ImGui::SameLine();
    ImGui::Checkbox("Steering axis and instant centres", &m_showGeometry);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Rear view: each side's line from the contact point through the front-view instant centre; where they cross the\n"
                          "centre plane is the roll centre. Side view: the line to the side-view instant centre (anti-dive / anti-squat).\n"
                          "Both views: the steering axis (kingpin, or the virtual one) down to the road.");
    }

    PoseLinkage();
    const std::array<double, 2> offsets = {m_car->corners[m_axle * 2].position.y, m_car->corners[m_axle * 2 + 1].position.y};
    const bool fit = previousAxle != m_axle || ImGui::IsWindowAppearing();

    // The readout: both wheels of the axle as the linkage has them.
    if (ImGui::BeginTable("pose", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
    {
        for (const char* column : {"wheel", "travel mm", "camber deg", "toe-in deg", "half-track mm", "KPI deg", "caster deg", "scrub mm", "trail mm", "roll centre mm"})
        {
            ImGui::TableSetupColumn(column);
        }
        ImGui::TableHeadersRow();
        for (int side = 0; side < 2; ++side)
        {
            KinematicOutputs out;
            if (m_solid)
            {
                m_solid->ComputeOutputs(side, out);
            }
            else
            {
                suspension::ComputeOutputs(*m_kinematics[side], out);
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kWheelNames[m_axle * 2 + side]);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", (m_solid ? m_solid->Travel(side) : m_kinematics[side]->Travel()) * 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", out.camber * kDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", out.toe * kDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", out.halfTrackChange * 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", out.kingpinInclination * kDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", out.caster * kDegrees);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", out.scrubRadius * 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", out.casterTrail * 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", out.rollCenterHeight * 1000.0);
        }
        ImGui::EndTable();
    }
    if (m_poseFailed)
    {
        ImGui::TextColored(ui_colors::kTextWarning, "The linkage cannot reach this pose: it stops where it can.");
    }

    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::max(ImGui::GetContentRegionAvail().y, 360.0f);
    const ImVec2 large(width * 0.5f - 4.0f, height);
    const ImVec2 small(width * 0.5f - 4.0f, height * 0.5f - 4.0f);
    DrawLinkageView("Rear view (from behind)", View::Rear, m_kinematics, m_solid.get(), offsets, m_showGeometry, fit, large);
    ImGui::SameLine();
    ImGui::BeginGroup();
    DrawLinkageView("Side view (left wheel)", View::Side, m_kinematics, m_solid.get(), offsets, m_showGeometry, fit, small);
    DrawLinkageView("Top view", View::Top, m_kinematics, m_solid.get(), offsets, m_showGeometry, fit, small);
    ImGui::EndGroup();
}
}
