#include "flex_ring_tyre_panel.h"

#include <engine/editor/ui/framework/editor_context.h>
#include <engine/tyre/flex_ring/flex_ring_modal.h>
#include <engine/tyre/flex_ring/flex_ring_rig.h>

#include <IconsPhosphor.h>
#include <glm/geometric.hpp>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>

namespace me
{

namespace
{
using namespace tyre::flexring;
using Clock = std::chrono::steady_clock;
constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kHistorySeconds = 3.0;
constexpr double kLiveStep = 0.001;
constexpr double kWheelInertia = 1.6;

// A line on an ImPlot plot from points.
struct Line
{
    std::vector<double> x, y;
    void Add(double a, double b)
    {
        x.push_back(a);
        y.push_back(b);
    }
    void Plot(const char* label, const ImVec4& colour, float weight = 1.5f) const
    {
        if (x.empty())
        {
            return;
        }
        ImPlotSpec spec;
        spec.LineColor = colour;
        spec.LineWeight = weight;
        ImPlot::PlotLine(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
    }
};

// Blocks coloured by ground pressure (0 .. maxPressure over the Viridis map).
struct PressureDots
{
    std::vector<double> x, y;
    std::vector<ImU32> colours;
    void Add(double a, double b, double pressure, double maxPressure)
    {
        x.push_back(a);
        y.push_back(b);
        const float t = static_cast<float>(std::clamp(pressure / std::max(maxPressure, 1.0), 0.0, 1.0));
        colours.push_back(ImGui::GetColorU32(ImPlot::SampleColormap(t, ImPlotColormap_Viridis)));
    }
    void Plot(const char* label, float size) const
    {
        if (x.empty())
        {
            return;
        }
        ImPlotSpec spec;
        spec.Marker = ImPlotMarker_Circle;
        spec.MarkerSize = size;
        spec.MarkerFillColors = const_cast<ImU32*>(colours.data());
        spec.MarkerLineColors = const_cast<ImU32*>(colours.data());
        spec.LineWeight = 0.0f;
        ImPlot::PlotScatter(label, x.data(), y.data(), static_cast<int>(x.size()), spec);
    }
};

const ImVec4 kRoadColour(0.55f, 0.55f, 0.55f, 1.0f);
const ImVec4 kBeltColour(0.95f, 0.65f, 0.25f, 1.0f);
const ImVec4 kTreadColour(0.35f, 0.75f, 0.95f, 1.0f);
const ImVec4 kRimColour(0.7f, 0.7f, 0.75f, 1.0f);
const ImVec4 kRestColour(0.5f, 0.5f, 0.5f, 0.5f);
const ImVec4 kSlideColour(0.95f, 0.25f, 0.25f, 1.0f);

// Axis limits around a centre with the same scale on both axes for a plot of `size` (ImPlot's Equal flag
// cannot hold with both ranges forced each frame).
void SetupEqualLimits(double cx, double cy, double minHalfWidth, double minHalfHeight, const ImVec2& size)
{
    const double aspect = std::max(size.y - 55.0, 20.0) / std::max(size.x - 65.0, 20.0);
    const double halfWidth = std::max(minHalfWidth, minHalfHeight / aspect);
    const double halfHeight = halfWidth * aspect;
    ImPlot::SetupAxesLimits(cx - halfWidth, cx + halfWidth, cy - halfHeight, cy + halfHeight, ImPlotCond_Always);
}

void PushHistory(std::deque<double>& d, double v)
{
    d.push_back(v);
}

// The live rig's road: periodic transversal cleats, one longitudinal cleat along x, or waves.
std::unique_ptr<Road> MakeLiveRoad(int kind, double heightMm, double widthMm)
{
    const double h = heightMm / 1000.0;
    const double w = widthMm / 1000.0;
    switch (kind)
    {
    case 1:
    {
        CleatGeometry g;
        g.width = w;
        g.height = h;
        const double period = 2.5;
        auto cleat = std::make_shared<CleatRoad>(0.0, g);
        // The first cleat 1.25 m ahead of the start, then one every 2.5 m.
        return std::make_unique<FunctionRoad>([cleat, period](double x, double y) {
            const double shifted = x - 0.5 * period;
            const double u = shifted - period * std::round(shifted / period);
            return cleat->Height(u, y);
        });
    }
    case 2:
    {
        CleatGeometry g;
        g.width = w;
        g.height = h;
        g.direction[0] = 0.0;
        g.direction[1] = 1.0;
        return std::make_unique<CleatRoad>(0.0, g);
    }
    case 3:
        return std::make_unique<FunctionRoad>([h](double x, double) {
            return 0.5 * h * (1.0 - std::cos(2.0 * std::numbers::pi * x / 1.0));
        });
    default:
        return std::make_unique<FlatRoad>(0.0);
    }
}
}

FlexRingTyrePanel::FlexRingTyrePanel()
    : EditorPanel("flex_ring_tyre", "Flex Ring Tyre", ICON_PH_TIRE), m_data(MakeDefaultData())
{
}

FlexRingTyrePanel::~FlexRingTyrePanel()
{
    if (m_pre)
    {
        m_pre->cancel = true;
    }
    if (m_tests)
    {
        m_tests->cancel = true;
    }
    WaitForWork();
}

void FlexRingTyrePanel::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(1200.0f, 820.0f), ImGuiCond_FirstUseEver);
}

void FlexRingTyrePanel::OnGui(EditorContext& context)
{
    static_cast<void>(context);
    DrawContents(ImGui::GetIO().DeltaTime);
}

void FlexRingTyrePanel::WaitForWork()
{
    if (m_pre)
    {
        m_pre->result.wait();
    }
    if (m_tests)
    {
        m_tests->result.wait();
    }
    PollWork();
}

void FlexRingTyrePanel::StartPreprocess()
{
    if (m_pre)
    {
        return;
    }
    auto run = std::make_unique<PreRun>();
    PreRun* state = run.get();
    const FlexRingData data = m_data;
    const bool fit = m_fit;
    state->result = RunAsync(TaskPriority::Low, [state, data, fit] {
        PreprocessOptions options;
        options.fitStatic = fit;
        options.fitModal = fit;
        options.cancel = &state->cancel;
        options.progress = [state](const std::string& stage, double progress) {
            state->progress = progress;
            const std::lock_guard lock(state->mutex);
            state->stage = stage;
        };
        return Preprocess(data, options);
    });
    m_pre = std::move(run);
    m_error.clear();
}

void FlexRingTyrePanel::StartTests(bool quick)
{
    if (m_tests || !m_model.has_value())
    {
        return;
    }
    auto run = std::make_unique<TestRun>();
    TestRun* state = run.get();
    const FlexRingData data = m_data;
    const std::string directory = m_outputDirectory;
    state->result = RunAsync(TaskPriority::Low, [state, data, directory, quick] {
        ReportOptions options;
        options.fit = true;
        options.quick = quick;
        options.modal = options.statics = options.sweeps = options.cleat = options.benchmark = true;
        options.outputDirectory = directory;
        options.cancel = &state->cancel;
        options.log = [state](const std::string& line) {
            const std::lock_guard lock(state->mutex);
            state->log.push_back(line);
        };
        return RunReport(data, options);
    });
    m_tests = std::move(run);
    m_testLog.clear();
}

void FlexRingTyrePanel::PollWork()
{
    if (m_pre && m_pre->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            m_model = m_pre->result.get();
            m_dataChanged = false;
            m_tyre.reset();
        }
        catch (const std::exception& e)
        {
            m_error = std::string("pre-processing failed: ") + e.what();
        }
        m_pre.reset();
    }
    if (m_tests)
    {
        {
            const std::lock_guard lock(m_tests->mutex);
            m_testLog = m_tests->log;
        }
        if (m_tests->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                m_report = m_tests->result.get();
            }
            catch (const std::exception& e)
            {
                m_error = std::string("the tests failed: ") + e.what();
            }
            m_tests.reset();
        }
    }
}

void FlexRingTyrePanel::DrawContents(double frameSeconds)
{
    PollWork();
    if (!m_model.has_value() && !m_pre)
    {
        StartPreprocess();
    }
    DrawHeader();
    if (!m_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_error.c_str());
    }
    if (ImGui::BeginTabBar("##flexring"))
    {
        const auto tab = [&](const char* name, Tab id) {
            const ImGuiTabItemFlags flags = m_requestedTab == id ? ImGuiTabItemFlags_SetSelected : 0;
            return ImGui::BeginTabItem(name, nullptr, flags);
        };
        if (tab("Live Rig", LiveTab))
        {
            DrawLiveTab(frameSeconds);
            ImGui::EndTabItem();
        }
        if (tab("Parameters", ParametersTab))
        {
            DrawParametersTab();
            ImGui::EndTabItem();
        }
        if (tab("Modes", ModesTab))
        {
            DrawModesTab();
            ImGui::EndTabItem();
        }
        if (tab("Tests", TestsTab))
        {
            DrawTestsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    m_requestedTab = -1;
}

void FlexRingTyrePanel::DrawHeader()
{
    ImGui::TextUnformatted(m_data.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_data.source.c_str());
    char path[512];
    std::snprintf(path, sizeof(path), "%s", m_dataPath.c_str());
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputTextWithHint("##path", "data file (.tir)", path, sizeof(path)))
    {
        m_dataPath = path;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_FOLDER_OPEN " Load"))
    {
        try
        {
            const DataReadResult read = ReadDataFile(m_dataPath);
            m_data = read.data;
            m_dataChanged = true;
            m_dataMessage = read.unknownKeys.empty() ? "loaded" : "loaded; " + std::to_string(read.unknownKeys.size()) + " unknown keys";
        }
        catch (const std::exception& e)
        {
            m_dataMessage = e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_FLOPPY_DISK " Save"))
    {
        try
        {
            WriteDataFile(m_data, m_dataPath);
            m_dataMessage = "saved";
        }
        catch (const std::exception& e)
        {
            m_dataMessage = e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("R34 default"))
    {
        m_data = MakeDefaultData();
        m_dataChanged = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Fit", &m_fit);
    ImGui::SameLine();
    if (m_pre)
    {
        std::string stage;
        {
            const std::lock_guard lock(m_pre->mutex);
            stage = m_pre->stage;
        }
        ImGui::ProgressBar(static_cast<float>(m_pre->progress.load()), ImVec2(220.0f, 0.0f), stage.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_pre->cancel = true;
        }
    }
    else
    {
        if (ImGui::Button(m_dataChanged ? ICON_PH_GEAR " Pre-process *" : ICON_PH_GEAR " Pre-process"))
        {
            StartPreprocess();
        }
    }
    if (!m_dataMessage.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_dataMessage.c_str());
    }
    if (m_model.has_value())
    {
        const PreprocessResult& r = *m_model;
        ImGui::TextDisabled("Model: %d segments x %zu blocks, belt %.1f mm, outer %.1f mm, belt mass %.2f kg; pre-processed in %.2f s (%s)", r.parameters.segments,
                            r.parameters.blocks.size(), r.beltRadius * 1000.0, r.outerRadius * 1000.0, r.freeMass, r.seconds, r.converged ? "fitted" : "fit incomplete");
    }
}

void FlexRingTyrePanel::DrawParametersTab()
{
    if (ImGui::BeginChild("##params", ImVec2(0.0f, 0.0f)))
    {
        std::string group;
        bool open = false;
        for (const FieldInfo& f : Fields())
        {
            if (group != f.group)
            {
                if (open)
                {
                    ImGui::EndTable();
                }
                group = f.group;
                open = ImGui::CollapsingHeader(f.group, std::string(f.group) == "Size and geometry" ? ImGuiTreeNodeFlags_DefaultOpen : 0) &&
                       ImGui::BeginTable(f.group, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
                if (open)
                {
                    ImGui::TableSetupColumn("item", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("unit", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                }
            }
            if (!open)
            {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.name);
            if (!f.ftire)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(MiniEngine)");
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemHovered())
            {
                ImGui::SetItemTooltip("%s", f.description);
            }
            ImGui::TableNextColumn();
            ImGui::PushID(f.name);
            ImGui::SetNextItemWidth(-1.0f);
            if (f.kind == FieldKind::Text)
            {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "%s", (m_data.*(f.text)).c_str());
                if (ImGui::InputText("##v", buffer, sizeof(buffer)))
                {
                    m_data.*(f.text) = buffer;
                    m_dataChanged = true;
                }
            }
            else
            {
                double value = m_data.*(f.real);
                if (ImGui::InputDouble("##v", &value, 0.0, 0.0, f.integer ? "%.0f" : "%.6g"))
                {
                    SetField(m_data, f.name, std::clamp(value, f.minValue, f.maxValue));
                    m_dataChanged = true;
                }
            }
            ImGui::SetItemTooltip("%s", f.description);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", f.unit);
        }
        if (open)
        {
            ImGui::EndTable();
        }

        if (m_model.has_value())
        {
            ImGui::SeparatorText("Fit");
            if (ImGui::BeginTable("##targets", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders))
            {
                ImGui::TableSetupColumn("target");
                ImGui::TableSetupColumn("data");
                ImGui::TableSetupColumn("model");
                ImGui::TableSetupColumn("error");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (const FitTarget& t : m_model->targets)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s [%s]", t.name.c_str(), t.unit.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.6g", t.target);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.6g", t.achieved);
                    ImGui::TableNextColumn();
                    const double e = 100.0 * t.RelativeError();
                    const bool bad = t.fitted && std::abs(e) > 0.5;
                    ImGui::TextColored(bad ? ImVec4(1.0f, 0.55f, 0.3f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%+.3f %%", e);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", t.fitted ? "fitted" : "reported");
                }
                ImGui::EndTable();
            }
            for (const std::string& n : m_model->notes)
            {
                ImGui::TextWrapped(ICON_PH_INFO " %s", n.c_str());
            }
            ImGui::SeparatorText("Pre-processed model");
            if (ImGui::BeginTable("##internal", 3, ImGuiTableFlags_RowBg))
            {
                for (const ParameterRow& r : DescribeParameters(m_model->parameters))
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.6g", r.value);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", r.unit.c_str());
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::EndChild();
}

void FlexRingTyrePanel::DrawModesTab()
{
    if (!m_model.has_value())
    {
        ImGui::TextDisabled("Pre-process the tyre first.");
        return;
    }
    const ModalResult& m = m_model->modal;
    ImGui::Text("Unloaded tyre on a fixed rim (FTire fig. 6.1): f1 %.2f  f2 %.2f  f3 %.2f  f4 %.2f  f5 %.2f  f6 %.2f Hz", m.f1, m.f2, m.f3, m.f4, m.f5, m.f6);
    ImGui::TextDisabled("Linearization: out-of-subspace residual %.2e, asymmetry %.2e", m.subspaceResidual, m.asymmetry);
    if (ImGui::BeginTable("##modes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("wave number");
        ImGui::TableSetupColumn("frequency Hz");
        ImGui::TableSetupColumn("damping");
        ImGui::TableSetupColumn("radial");
        ImGui::TableSetupColumn("tangential");
        ImGui::TableSetupColumn("lateral");
        ImGui::TableSetupColumn("torsion");
        ImGui::TableHeadersRow();
        for (const ModeInfo& mode : m.modes)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d %s", mode.waveNumber, mode.InPlane() ? "in-plane" : "out-of-plane");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", mode.frequency);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", mode.damping);
            for (int i = 0; i < 4; ++i)
            {
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", mode.share[i]);
            }
        }
        ImGui::EndTable();
    }
}

void FlexRingTyrePanel::ResetLive()
{
    m_tyre.reset();
    if (!m_model.has_value())
    {
        return;
    }
    m_tyre = std::make_unique<FlexRingTyre>(m_model->parameters);
    BuildLiveRoad();
    m_tyre->SetRoad(m_road.get());
    if (m_live.pressureBar > 0.0f)
    {
        m_tyre->SetPressure(m_live.pressureBar * 1.0e5);
    }
    m_tyre->SetFrictionScale(m_live.frictionScale);
    RigPose pose;
    pose.camber = m_live.camberDeg * kDeg;
    m_touch = RestTouchHeight(*m_tyre, pose);
    const double rate = m_data.firstDeflection > 0.0 ? m_data.statWheelLoadAtFirstDefl / (m_data.firstDeflection / 1000.0) : 2.5e5;
    m_kv = std::max(rate, 1.0e4);
    m_deflection = std::clamp(m_live.load / m_kv, 0.0, 0.08);
    m_x = 0.0;
    m_y = 0.0;
    pose.heading = 0.0;
    // Settle quasi-statically at the guessed deflection; the load control finishes the job rolling.
    m_tyre->Reset(MakeRim(pose, m_touch, 0.0, Vec3(0.0), 0.0));
    for (double d = 0.0025; d < m_deflection; d += 0.0025)
    {
        m_tyre->SettleStatic(MakeRim(pose, m_touch - d, 0.0, Vec3(0.0), 0.0), 600);
    }
    m_tyre->SettleStatic(MakeRim(pose, m_touch - m_deflection, 0.0, Vec3(0.0), 0.0), 600);
    m_filteredLoad = m_tyre->Contact().normalForce;
    m_spin = 0.0;
    m_spinAngle = 0.0;
    m_speed = 0.0;
    m_liveTime = 0.0;
    m_history = History{};
}

void FlexRingTyrePanel::BuildLiveRoad()
{
    m_road = MakeLiveRoad(m_live.road, m_live.cleatHeightMm, m_live.cleatWidthMm);
    m_roadBuilt = m_live.road;
    if (m_tyre)
    {
        m_tyre->SetRoad(m_road.get());
    }
}

void FlexRingTyrePanel::StepLive(double frameSeconds)
{
    if (!m_model.has_value())
    {
        return;
    }
    if (!m_tyre)
    {
        ResetLive();
    }
    if (!m_tyre || !m_live.running)
    {
        return;
    }
    const auto start = Clock::now();
    const double budget = 0.012; // wall seconds per frame
    double toSimulate = std::min(frameSeconds, 0.05) * m_live.timeScale;
    const FlexRingParameters& p = m_model->parameters;
    double simulated = 0.0;
    while (toSimulate >= kLiveStep * 0.5)
    {
        if (std::chrono::duration<double>(Clock::now() - start).count() > budget)
        {
            break;
        }
        const double target = m_live.mode == 1 ? m_live.speedKmh / 3.6 : 0.0;
        const double accel = 40.0 * kLiveStep;
        m_speed += std::clamp(target - m_speed, -accel, accel);
        m_x += m_speed * kLiveStep;
        RigPose pose;
        pose.x = m_x;
        pose.y = m_y;
        pose.heading = m_live.slipAngleDeg * kDeg;
        pose.camber = m_live.camberDeg * kDeg;
        const double re = p.beltRadius;
        const double rolling = m_speed * std::cos(pose.heading) / re;
        if (m_speed < 1.0 || !m_live.freeRolling)
        {
            m_spin = m_live.freeRolling ? rolling : (1.0 + m_live.slipRatio) * rolling;
        }
        else
        {
            const double moment = glm::dot(m_tyre->LastWrench().moment, m_tyre->Rim().rotation[1]);
            m_spin = std::clamp(m_spin + kLiveStep * moment / kWheelInertia, 0.5 * rolling, 1.5 * rolling);
        }
        m_spinAngle += m_spin * kLiveStep;
        const RimState rim = MakeRim(pose, m_touch - m_deflection, m_spinAngle, Vec3(m_speed, 0.0, 0.0), m_spin);
        const Wrench w = m_tyre->Advance(rim, kLiveStep);
        const double load = m_tyre->Contact().normalForce;
        if (!std::isfinite(w.force.z) || !std::isfinite(load))
        {
            m_error = "the live rig diverged; press Reset";
            m_live.running = false;
            m_tyre.reset();
            break;
        }
        m_filteredLoad += (load - m_filteredLoad) * (kLiveStep / 0.02);
        m_deflection = std::clamp(m_deflection + 0.03 * (m_live.load - m_filteredLoad) / m_kv, 0.0, 0.1);
        m_liveTime += kLiveStep;
        simulated += kLiveStep;
        toSimulate -= kLiveStep;
        const RoadForces f = ToRoadAxes(w, pose.heading);
        PushHistory(m_history.t, m_liveTime);
        PushHistory(m_history.fx, f.fx);
        PushHistory(m_history.fy, f.fy);
        PushHistory(m_history.fz, f.fz);
        PushHistory(m_history.mz, f.mz);
        while (!m_history.t.empty() && m_history.t.front() < m_liveTime - kHistorySeconds)
        {
            m_history.t.pop_front();
            m_history.fx.pop_front();
            m_history.fy.pop_front();
            m_history.fz.pop_front();
            m_history.mz.pop_front();
        }
    }
    if (simulated > 0.0)
    {
        const double cpu = std::chrono::duration<double>(Clock::now() - start).count();
        const double rate = cpu / simulated;
        m_cpuPerSecond = m_cpuPerSecond > 0.0 ? 0.9 * m_cpuPerSecond + 0.1 * rate : rate;
    }
}

void FlexRingTyrePanel::DrawLiveTab(double frameSeconds)
{
    if (!m_model.has_value())
    {
        ImGui::TextDisabled("Pre-processing the tyre...");
        return;
    }
    StepLive(frameSeconds);
    LiveSettings& s = m_live;
    if (ImGui::BeginTable("##live", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableSetupColumn("views", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushItemWidth(-130.0f);
        if (ImGui::Button(s.running ? ICON_PH_PAUSE " Pause" : ICON_PH_PLAY " Run"))
        {
            s.running = !s.running;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE " Reset"))
        {
            ResetLive();
            m_error.clear();
            s.running = true;
        }
        ImGui::Combo("Mode", &s.mode, "Standing\0Rolling\0");
        ImGui::SliderFloat("Load N", &s.load, 500.0f, 9000.0f, "%.0f");
        ImGui::SliderFloat("Speed km/h", &s.speedKmh, 0.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("Slip angle deg", &s.slipAngleDeg, -15.0f, 15.0f, "%.1f");
        ImGui::Checkbox("Free rolling", &s.freeRolling);
        if (!s.freeRolling)
        {
            ImGui::SliderFloat("Slip ratio", &s.slipRatio, -0.5f, 0.5f, "%.3f");
        }
        if (ImGui::SliderFloat("Camber deg", &s.camberDeg, -10.0f, 10.0f, "%.1f"))
        {
            // The touch height changes with camber.
            if (m_tyre)
            {
                RigPose pose;
                pose.camber = s.camberDeg * kDeg;
                FlexRingTyre probe(m_model->parameters);
                m_touch = RestTouchHeight(probe, pose);
            }
        }
        if (ImGui::Combo("Road", &s.road, "Flat\0Transversal cleats (every 2.5 m)\0Longitudinal cleat\0Waves (1 m)\0") || m_roadBuilt != s.road)
        {
            BuildLiveRoad();
        }
        if (s.road != 0)
        {
            if (ImGui::SliderFloat("Height mm", &s.cleatHeightMm, 1.0f, 40.0f, "%.0f") | ImGui::SliderFloat("Width mm", &s.cleatWidthMm, 5.0f, 200.0f, "%.0f"))
            {
                BuildLiveRoad();
            }
        }
        if (ImGui::SliderFloat("Pressure bar", &s.pressureBar, 0.0f, 4.0f, s.pressureBar > 0.0f ? "%.2f" : "data") && m_tyre)
        {
            m_tyre->SetPressure((s.pressureBar > 0.0f ? s.pressureBar : m_data.inflationPressure) * 1.0e5);
        }
        if (ImGui::SliderFloat("Friction", &s.frictionScale, 0.1f, 1.5f, "%.2f") && m_tyre)
        {
            m_tyre->SetFrictionScale(s.frictionScale);
        }
        ImGui::SliderFloat("Time scale", &s.timeScale, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Magnify", &s.magnification, 1.0f, 20.0f, "x%.1f");
        ImGui::PopItemWidth();
        ImGui::Separator();
        if (m_tyre && !m_history.t.empty())
        {
            const ContactStats& c = m_tyre->Contact();
            ImGui::Text("t %.2f s, v %.1f km/h, spin %.2f rad/s", m_liveTime, m_speed * 3.6, m_spin);
            ImGui::Text("Fx %8.1f N   Fy %8.1f N", m_history.fx.back(), m_history.fy.back());
            ImGui::Text("Fz %8.1f N   Mz %8.2f N m", m_history.fz.back(), m_history.mz.back());
            ImGui::Text("Road load %.1f N, deflection %.2f mm", c.normalForce, m_deflection * 1000.0);
            ImGui::Text("Patch %.0f x %.0f mm, %.1f cm^2", c.length * 1000.0, c.width * 1000.0, c.area * 1.0e4);
            ImGui::Text("Pressure mean %.2f, max %.2f bar", c.meanPressure / 1.0e5, c.maxPressure / 1.0e5);
            ImGui::Text("Blocks %d, sliding %d (%.0f %%)", c.blocks, c.sliding, c.blocks > 0 ? 100.0 * c.sliding / c.blocks : 0.0);
            if (m_speed > 1.0)
            {
                ImGui::Text("Effective radius %.2f mm", m_speed / std::max(m_spin, 1.0e-6) * 1000.0);
            }
            ImGui::Text("Substeps %d, CPU %.2f s per simulated s", m_tyre->LastStep().substeps, m_cpuPerSecond);
            if (m_cpuPerSecond * s.timeScale > 1.0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "slower than asked (%.0f %% of real time)", 100.0 / std::max(m_cpuPerSecond, 1.0e-9));
            }
        }
        ImGui::TableNextColumn();
        const float width = ImGui::GetContentRegionAvail().x;
        const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        DrawSideView(ImVec2(half, 300.0f));
        ImGui::SameLine();
        DrawFrontView(ImVec2(half, 300.0f));
        DrawFootprint(ImVec2(half, 240.0f));
        ImGui::SameLine();
        if (ImPlot::BeginPlot("Forces", ImVec2(half, 240.0f)))
        {
            ImPlot::SetupAxes("t (s)", "N, N m", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            const auto plot = [&](const char* label, const std::deque<double>& d) {
                std::vector<double> t(m_history.t.begin(), m_history.t.end());
                std::vector<double> v(d.begin(), d.end());
                if (!t.empty())
                {
                    ImPlot::PlotLine(label, t.data(), v.data(), static_cast<int>(t.size()));
                }
            };
            plot("Fx", m_history.fx);
            plot("Fy", m_history.fy);
            plot("Fz", m_history.fz);
            plot("Mz", m_history.mz);
            ImPlot::EndPlot();
        }
        ImGui::EndTable();
    }
}

void FlexRingTyrePanel::DrawSideView(const ImVec2& size)
{
    if (!m_tyre || !std::isfinite(m_tyre->Rim().position.z) || !ImPlot::BeginPlot("Side (deformation magnified)", size, ImPlotFlags_NoMenus))
    {
        return;
    }
    const FlexRingTyre& t = *m_tyre;
    const RimState& rim = t.Rim();
    const double mag = m_live.magnification;
    const double cx = rim.position.x;
    const double cz = rim.position.z;
    ImPlot::SetupAxes("x (m)", "z (m)");
    SetupEqualLimits(cx, cz - 0.02, 0.42, 0.37, size);
    Line road, rimLine, rest, belt, tread;
    for (int i = 0; i <= 200; ++i)
    {
        const double x = cx - 0.6 + 1.2 * i / 200.0;
        road.Add(x, m_road ? m_road->Height(x, rim.position.y) : 0.0);
    }
    const FlexRingParameters& p = t.Parameters();
    for (int i = 0; i <= 64; ++i)
    {
        const double a = 2.0 * std::numbers::pi * i / 64.0;
        rimLine.Add(cx + p.rimRadius * std::cos(a), cz + p.rimRadius * std::sin(a));
    }
    const int n = t.Segments();
    for (int k = 0; k <= n; ++k)
    {
        const int i = k % n;
        const Vec3 r = t.NodeRestPosition(i);
        const Vec3 x = t.NodePositions()[static_cast<size_t>(i)];
        rest.Add(r.x, r.z);
        const Vec3 m = r + mag * (x - r);
        belt.Add(m.x, m.z);
    }
    for (int k = 0; k < n; ++k)
    {
        const Vec3 r0 = t.NodeRestPosition(k);
        const Vec3 r1 = t.NodeRestPosition((k + 1) % n);
        for (int j = 0; j < 4; ++j)
        {
            const double sigma = j / 4.0;
            const Vec3 c = t.SurfacePoint(k, sigma, 0.0, false);
            const Vec3 tr = t.SurfacePoint(k, sigma, 0.0, true);
            const Vec3 restC = r0 + (r1 - r0) * sigma;
            const Vec3 m = restC + mag * (c - restC) + (tr - c);
            tread.Add(m.x, m.z);
        }
    }
    if (!tread.x.empty())
    {
        tread.Add(tread.x.front(), tread.y.front());
    }
    road.Plot("road", kRoadColour, 2.0f);
    rimLine.Plot("rim", kRimColour, 1.0f);
    rest.Plot("belt at rest", kRestColour, 1.0f);
    belt.Plot("belt", kBeltColour, 2.0f);
    tread.Plot("tread", kTreadColour, 1.5f);
    PressureDots dots;
    Line sliding;
    const double maxP = std::max(t.Contact().maxPressure, 1.0);
    for (const BlockView& b : t.Blocks())
    {
        if (b.contact)
        {
            dots.Add(b.tip.x, b.tip.z, b.groundPressure, maxP);
        }
    }
    dots.Plot("blocks", 2.5f);
    ImPlot::EndPlot();
}

void FlexRingTyrePanel::DrawFrontView(const ImVec2& size)
{
    if (!m_tyre || !std::isfinite(m_tyre->Rim().position.z) || !ImPlot::BeginPlot("Front: section at the lowest node, lateral bending magnified", size, ImPlotFlags_NoMenus))
    {
        return;
    }
    const FlexRingTyre& t = *m_tyre;
    const RimState& rim = t.Rim();
    const FlexRingParameters& p = t.Parameters();
    const int n = t.Segments();
    int lowest = 0;
    for (int k = 1; k < n; ++k)
    {
        if (t.NodePositions()[static_cast<size_t>(k)].z < t.NodePositions()[static_cast<size_t>(lowest)].z)
        {
            lowest = k;
        }
    }
    const double mag = m_live.magnification;
    const double cy = rim.position.y;
    const double cz = rim.position.z - p.outerRadius + 0.02;
    ImPlot::SetupAxes("y (m)", "z (m)");
    SetupEqualLimits(cy, cz, 0.15, 0.05, size);
    // The section as it is, and its departure from the straight line through it magnified.
    constexpr int kSamples = 41;
    std::vector<Vec3> belt(kSamples), tread(kSamples);
    double sy = 0.0, sz = 0.0, syy = 0.0, syz = 0.0;
    for (int j = 0; j < kSamples; ++j)
    {
        const double s = -0.5 * p.treadWidth + p.treadWidth * j / (kSamples - 1);
        belt[j] = t.SurfacePoint(lowest, 0.0, s, false);
        tread[j] = t.SurfacePoint(lowest, 0.0, s, true);
        sy += belt[j].y;
        sz += belt[j].z;
        syy += belt[j].y * belt[j].y;
        syz += belt[j].y * belt[j].z;
    }
    const double slope = (kSamples * syz - sy * sz) / std::max(kSamples * syy - sy * sy, 1.0e-12);
    const double offset = (sz - slope * sy) / kSamples;
    Line beltLine, treadLine, road, straight;
    for (int j = 0; j < kSamples; ++j)
    {
        const double line = offset + slope * belt[j].y;
        const double z = line + mag * (belt[j].z - line);
        beltLine.Add(belt[j].y, z);
        treadLine.Add(tread[j].y, z + (tread[j].z - belt[j].z));
        straight.Add(belt[j].y, line);
        road.Add(tread[j].y, m_road ? m_road->Height(tread[j].x, tread[j].y) : 0.0);
    }
    road.Plot("road", kRoadColour, 2.0f);
    straight.Plot("straight", kRestColour, 1.0f);
    beltLine.Plot("belt", kBeltColour, 2.0f);
    treadLine.Plot("tread", kTreadColour, 1.5f);
    PressureDots dots;
    const double maxP = std::max(t.Contact().maxPressure, 1.0);
    const int nb = t.BlocksPerSegment();
    for (int k = lowest - 1; k <= lowest; ++k)
    {
        const int seg = (k + n) % n;
        for (int b = 0; b < nb; ++b)
        {
            const BlockView& v = t.Blocks()[static_cast<size_t>(seg) * nb + b];
            if (v.contact)
            {
                dots.Add(v.tip.y, v.tip.z, v.groundPressure, maxP);
            }
        }
    }
    dots.Plot("blocks", 3.0f);
    ImPlot::EndPlot();
}

void FlexRingTyrePanel::DrawFootprint(const ImVec2& size)
{
    if (!m_tyre || !std::isfinite(m_tyre->Rim().position.z) || !ImPlot::BeginPlot("Footprint: ground pressure, sliding ringed", size, ImPlotFlags_NoMenus))
    {
        return;
    }
    const FlexRingTyre& t = *m_tyre;
    const RimState& rim = t.Rim();
    ImPlot::SetupAxes("x (mm)", "y (mm)");
    SetupEqualLimits(0.0, 0.0, 170.0, 125.0, size);
    PressureDots dots;
    std::vector<double> sx, sy;
    const double maxP = std::max(t.Contact().maxPressure, 1.0);
    for (const BlockView& b : t.Blocks())
    {
        if (!b.contact)
        {
            continue;
        }
        const double x = (b.tip.x - rim.position.x) * 1000.0;
        const double y = (b.tip.y - rim.position.y) * 1000.0;
        dots.Add(x, y, b.groundPressure, maxP);
        if (b.sliding)
        {
            sx.push_back(x);
            sy.push_back(y);
        }
    }
    dots.Plot("pressure", 3.5f);
    if (!sx.empty())
    {
        ImPlotSpec spec;
        spec.Marker = ImPlotMarker_Circle;
        spec.MarkerSize = 5.0f;
        spec.MarkerFillColor = ImVec4(0, 0, 0, 0);
        spec.MarkerLineColor = kSlideColour;
        spec.LineWeight = 1.0f;
        ImPlot::PlotScatter("sliding", sx.data(), sy.data(), static_cast<int>(sx.size()), spec);
    }
    ImPlot::PlotText(("max " + std::to_string(static_cast<int>(maxP / 1000.0)) + " kPa").c_str(), 120.0, 135.0);
    ImPlot::EndPlot();
}

void FlexRingTyrePanel::DrawTestsTab()
{
    if (!m_model.has_value())
    {
        ImGui::TextDisabled("Pre-process the tyre first.");
        return;
    }
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s", m_outputDirectory.c_str());
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputText("Output folder (CSV, JSON)", dir, sizeof(dir)))
    {
        m_outputDirectory = dir;
    }
    if (m_tests)
    {
        if (ImGui::Button("Cancel tests"))
        {
            m_tests->cancel = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("running...");
    }
    else
    {
        if (ImGui::Button(ICON_PH_FLASK " Run quick tests"))
        {
            StartTests(true);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_FLASK " Run full tests"))
        {
            StartTests(false);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("modes, statics, slip sweeps at 2/4/6/8 kN, cleats at 40/80/120 km/h, computing time");
    }
    if (ImGui::BeginChild("##log", ImVec2(0.0f, 110.0f), ImGuiChildFlags_Borders))
    {
        for (const std::string& line : m_testLog)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (m_tests)
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    if (!m_report.has_value())
    {
        return;
    }
    const ReportResult& r = *m_report;
    const float width = ImGui::GetContentRegionAvail().x;
    const float third = (width - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const ImVec2 size(third, 230.0f);
    if (ImPlot::BeginPlot("Fz(deflection)", size))
    {
        ImPlot::SetupAxes("deflection (mm)", "wheel load (N)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        Line l;
        for (const StaticPoint& s : r.statics)
        {
            l.Add(s.deflection * 1000.0, s.contact.normalForce);
        }
        l.Plot("model", kBeltColour);
        Line data;
        data.Add(m_data.firstDeflection, m_data.statWheelLoadAtFirstDefl);
        if (m_data.secondDeflection > 0.0)
        {
            data.Add(m_data.secondDeflection, m_data.statWheelLoadAtSecondDefl);
        }
        ImPlotSpec spec;
        spec.Marker = ImPlotMarker_Square;
        spec.MarkerSize = 5.0f;
        spec.LineWeight = 0.0f;
        ImPlot::PlotScatter("data", data.x.data(), data.y.data(), static_cast<int>(data.x.size()), spec);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    const auto sweepPlot = [&](const char* title, const char* xLabel, const char* yLabel, const char* prefix, auto xOf, auto yOf) {
        if (!ImPlot::BeginPlot(title, size))
        {
            return;
        }
        ImPlot::SetupAxes(xLabel, yLabel, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        for (const SweepSeries& s : r.sweeps)
        {
            if (s.name.rfind(prefix, 0) != 0)
            {
                continue;
            }
            std::vector<double> x, y;
            for (const SteadyPoint& pt : s.points)
            {
                x.push_back(xOf(pt));
                y.push_back(yOf(pt));
            }
            if (!x.empty())
            {
                ImPlot::PlotLine(s.name.c_str() + std::string(prefix).size(), x.data(), y.data(), static_cast<int>(x.size()));
            }
        }
        ImPlot::EndPlot();
    };
    sweepPlot("Fy(alpha)", "slip angle (deg)", "Fy (N)", "Fy_Mz_alpha ", [](const SteadyPoint& p) { return p.settings.slipAngle / kDeg; }, [](const SteadyPoint& p) { return p.forces.fy; });
    ImGui::SameLine();
    sweepPlot("Mz(alpha)", "slip angle (deg)", "Mz (N m)", "Fy_Mz_alpha ", [](const SteadyPoint& p) { return p.settings.slipAngle / kDeg; }, [](const SteadyPoint& p) { return p.forces.mz; });
    sweepPlot("Fx(kappa)", "slip ratio", "Fx (N)", "Fx_kappa ", [](const SteadyPoint& p) { return p.settings.slipRatio; }, [](const SteadyPoint& p) { return p.forces.fx; });
    ImGui::SameLine();
    sweepPlot("Combined: Fy(kappa) at 4 kN", "slip ratio", "Fy (N)", "combined ", [](const SteadyPoint& p) { return p.settings.slipRatio; }, [](const SteadyPoint& p) { return p.forces.fy; });
    ImGui::SameLine();
    sweepPlot("Camber: Fy(alpha) at 4 kN", "slip angle (deg)", "Fy (N)", "camber ", [](const SteadyPoint& p) { return p.settings.slipAngle / kDeg; }, [](const SteadyPoint& p) { return p.forces.fy; });
    const ImVec2 wide((width - ImGui::GetStyle().ItemSpacing.x) * 0.5f, 240.0f);
    const auto cleatPlot = [&](const char* title, bool vertical) {
        if (!ImPlot::BeginPlot(title, wide))
        {
            return;
        }
        ImPlot::SetupAxes("rim position past the cleat (m)", vertical ? "Fz (N)" : "Fx (N)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        for (const CleatSeries& c : r.cleats)
        {
            std::vector<double> x, y;
            for (const CleatSample& s : c.samples)
            {
                x.push_back(s.position);
                y.push_back(vertical ? s.forces.fz : s.forces.fx);
            }
            if (!x.empty())
            {
                ImPlot::PlotLine(c.name.c_str(), x.data(), y.data(), static_cast<int>(x.size()));
            }
        }
        ImPlot::EndPlot();
    };
    cleatPlot("Cleat: wheel load (fixed spindle, 4 kN)", true);
    ImGui::SameLine();
    cleatPlot("Cleat: longitudinal force", false);
    if (!r.benchmarks.empty() && ImGui::BeginTable("##bench", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders))
    {
        ImGui::TableSetupColumn("case");
        ImGui::TableSetupColumn("steps/s");
        ImGui::TableSetupColumn("us/step");
        ImGui::TableSetupColumn("RTF one tyre");
        ImGui::TableSetupColumn("RTF four tyres");
        ImGui::TableSetupColumn("blocks in contact");
        ImGui::TableHeadersRow();
        for (const BenchmarkCase& b : r.benchmarks)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", b.result.stepsPerSecond);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", b.result.microsecondsPerStep);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", b.result.secondsPerSimulatedSecond);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", b.result.parallelFourTyres);
            ImGui::TableNextColumn();
            ImGui::Text("%d", b.result.blocksInContact);
        }
        ImGui::EndTable();
    }
}

}
