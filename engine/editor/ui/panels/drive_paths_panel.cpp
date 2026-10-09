#include "drive_paths_panel.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>

#include <IconsPhosphor.h>
#include <ImGuizmo.h>
#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace me
{

namespace
{
// A recorded line gets a point every this many metres.
constexpr double kRecordSpacingMetres = 5.0;
// A click this close to a point (screen points at UI scale 1) selects it.
constexpr float kPointPickRadius = 9.0f;
// At most this many drive logs are listed.
constexpr size_t kListedLogs = 12;

constexpr ImU32 kPathColor = IM_COL32(90, 200, 230, 170);
constexpr ImU32 kSelectedPathColor = IM_COL32(255, 196, 64, 255);
constexpr ImU32 kPointColor = IM_COL32(255, 255, 255, 230);
constexpr ImU32 kLookaheadColor = IM_COL32(120, 255, 120, 255);

bool Project(const glm::mat4& viewProjection, const ImVec2& origin, const ImVec2& size, const glm::dvec3& world, ImVec2& screen)
{
    const glm::vec4 clip = viewProjection * glm::vec4(glm::vec3(world), 1.0f);
    if (clip.w <= 1.0e-4f)
    {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return false;
    }
    screen = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y);
    return true;
}

// A name no path has yet: `stem` with the first free number after it.
std::string FreeName(const std::vector<SceneDrivePath>& paths, const std::string& stem)
{
    for (int number = 1;; ++number)
    {
        std::string name = fmt::format("{}_{}", stem, number);
        if (std::none_of(paths.begin(), paths.end(), [&](const SceneDrivePath& path)
                         {
                             return path.name == name;
                         }))
        {
            return name;
        }
    }
}

// Where the mouse ray meets the level plane at `height`, if in front of the camera and within 5 km.
std::optional<glm::dvec3> MouseOnPlane(const glm::mat4& viewProjection, const ImVec2& origin, const ImVec2& size, double height)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float ndcX = (mouse.x - origin.x) / size.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouse.y - origin.y) / size.y * 2.0f;
    const glm::mat4 inverse = glm::inverse(viewProjection);
    const glm::vec4 nearClip = inverse * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 farClip = inverse * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::dvec3 nearWorld = glm::dvec3(glm::vec3(nearClip) / nearClip.w);
    const glm::dvec3 farWorld = glm::dvec3(glm::vec3(farClip) / farClip.w);
    const glm::dvec3 direction = glm::normalize(farWorld - nearWorld);
    if (std::abs(direction.y) < 1.0e-4)
    {
        return std::nullopt;
    }
    const double t = (height - nearWorld.y) / direction.y;
    if (t <= 0.0 || t > 5000.0)
    {
        return std::nullopt;
    }
    return nearWorld + direction * t;
}

std::string DescribeAutomation(const VehicleAutomationStatus& automation)
{
    if (automation.mode == VehicleAutomationMode::Path)
    {
        const char* state = automation.status == PathFollowerStatus::Running ? "Following" : PathFollowerStatusName(automation.status);
        std::string text = fmt::format(
            "{} '{}': {:.0f} / {:.0f} m, {:+.2f} m off, target {:.0f} km/h", state, automation.name, automation.distance, automation.length,
            automation.lateralError, automation.targetKmh);
        if (automation.laps > 1 || automation.lap > 0)
        {
            text += fmt::format(", lap {} of {}", std::min(automation.lap + 1, automation.laps), automation.laps);
        }
        return text;
    }
    if (automation.mode == VehicleAutomationMode::Replay)
    {
        return fmt::format(
            "{} '{}': frame {} of {}", automation.status == PathFollowerStatus::Running ? "Replaying" : "Replayed", automation.name,
            automation.frame, automation.frames);
    }
    return {};
}
}

DrivePathsPanel::DrivePathsPanel()
    : EditorPanel("drive_paths", "Drive Paths", ICON_PH_PATH)
{
}

const DrivePathTrack& DrivePathsPanel::TrackFor(size_t index, const SceneDrivePath& path)
{
    if (m_tracks.size() <= index)
    {
        m_tracks.resize(index + 1);
    }
    CachedTrack& cached = m_tracks[index];
    if (!(cached.path == path) || (cached.track.Empty() && path.points.size() >= 2))
    {
        cached.path = path;
        cached.track = BuildDrivePathTrack(path);
    }
    return cached.track;
}

void DrivePathsPanel::OnGui(EditorContext& context)
{
    IEditorWorld& scene = context.scene;
    std::vector<SceneDrivePath> paths = scene.GetDrivePaths();
    if (m_selectedPath >= static_cast<int>(paths.size()) || (m_selectedPath < 0 && !paths.empty()))
    {
        m_selectedPath = paths.empty() ? -1 : std::min(std::max(m_selectedPath, 0), static_cast<int>(paths.size()) - 1);
        m_selectedPoint = -1;
    }
    bool changed = false;
    DrawPathList(paths, changed);
    if (m_selectedPath >= 0)
    {
        DrawPathEditor(context, paths, changed);
    }
    DrawFollow(context, paths);
    DrawRecording(context, paths, changed);
    DrawDriveLog(context);
    if (changed)
    {
        scene.SetDrivePaths(std::move(paths));
    }
}

void DrivePathsPanel::DrawPathList(std::vector<SceneDrivePath>& paths, bool& changed)
{
    if (ImGui::Button(ICON_PH_PLUS " New Path"))
    {
        SceneDrivePath path;
        path.name = FreeName(paths, "path");
        paths.push_back(std::move(path));
        m_selectedPath = static_cast<int>(paths.size()) - 1;
        m_selectedPoint = -1;
        m_editPoints = true;
        m_placePoints = true;
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A new path, with placing points on: click in the viewport to add them.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selectedPath < 0);
    if (ImGui::Button(ICON_PH_TRASH " Delete Path") && m_selectedPath >= 0)
    {
        paths.erase(paths.begin() + m_selectedPath);
        m_tracks.clear();
        m_selectedPath = std::min(m_selectedPath, static_cast<int>(paths.size()) - 1);
        m_selectedPoint = -1;
        changed = true;
    }
    ImGui::EndDisabled();
    if (paths.empty())
    {
        ImGui::TextDisabled("The scene has no drive paths. Saved with the scene.");
        return;
    }
    if (ImGui::BeginListBox("##drive_paths", ImVec2(-FLT_MIN, static_cast<float>(std::min<size_t>(paths.size(), 6) + 0.25f) * ImGui::GetTextLineHeightWithSpacing())))
    {
        for (size_t index = 0; index < paths.size(); ++index)
        {
            const SceneDrivePath& path = paths[index];
            const DrivePathTrack& track = TrackFor(index, path);
            const std::string label = fmt::format(
                "{}  ({}, {} points, {:.0f} m)##{}", path.name, path.closed ? fmt::format("closed, {} laps", path.laps) : std::string("open"),
                path.points.size(), track.length, index);
            if (ImGui::Selectable(label.c_str(), m_selectedPath == static_cast<int>(index)))
            {
                if (m_selectedPath != static_cast<int>(index))
                {
                    m_selectedPoint = -1;
                }
                m_selectedPath = static_cast<int>(index);
            }
        }
        ImGui::EndListBox();
    }
}

void DrivePathsPanel::DrawPathEditor(EditorContext& context, std::vector<SceneDrivePath>& paths, bool& changed)
{
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    SceneDrivePath& path = paths[static_cast<size_t>(m_selectedPath)];
    ImGui::SeparatorText("Path");
    char name[128];
    std::snprintf(name, sizeof(name), "%s", path.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && path.name != name))
    {
        if (name[0] != '\0')
        {
            path.name = name;
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Closed", &path.closed);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The last point joins back to the first and the car laps it.");
    }
    if (path.closed)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        changed |= DragIntInRange("Laps", &path.laps, 1, 100);
    }
    changed |= DragFloatInRange("Speed (km/h)", &path.speedKmh, 0.0f, 400.0f, "%.0f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The speed at every point that sets none of its own (0 in its km/h column).");
    }

    ImGui::Checkbox("Edit Points", &m_editPoints);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The gizmo moves the selected point; clicking a point in the viewport selects it.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Place Points", &m_placePoints);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Clicking in the viewport adds a point after the selected one (or at the end), level with it:\n"
            "on the ground the path lies on. The first point goes level with the car, else at height 0.");
    }
    ImGui::BeginDisabled(!status.active);
    if (ImGui::Button(ICON_PH_MAP_PIN " Add at Car"))
    {
        const size_t at = m_selectedPoint >= 0 ? static_cast<size_t>(m_selectedPoint) + 1 : path.points.size();
        path.points.insert(path.points.begin() + static_cast<std::ptrdiff_t>(at), SceneDrivePathPoint{status.pose.position, 0.0f});
        m_selectedPoint = static_cast<int>(at);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Adds a point where the driven car is, after the selected one.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selectedPoint < 0 || m_selectedPoint >= static_cast<int>(path.points.size()));
    if (ImGui::Button(ICON_PH_TRASH " Delete Point"))
    {
        path.points.erase(path.points.begin() + m_selectedPoint);
        m_selectedPoint = std::min(m_selectedPoint, static_cast<int>(path.points.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float height = std::min(static_cast<float>(path.points.size()) + 1.5f, 10.0f) * ImGui::GetFrameHeightWithSpacing();
    if (!path.points.empty() && ImGui::BeginTable("##points", 3, flags, ImVec2(0.0f, height)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
        ImGui::TableSetupColumn("Position (m)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("km/h", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(path.points.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                SceneDrivePathPoint& point = path.points[static_cast<size_t>(row)];
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string label = std::to_string(row + 1);
                if (ImGui::Selectable(label.c_str(), m_selectedPoint == row, ImGuiSelectableFlags_AllowOverlap))
                {
                    m_selectedPoint = row;
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragScalarN("##position", ImGuiDataType_Double, &point.position.x, 3, 0.05f, nullptr, nullptr, "%.2f"))
                {
                    m_selectedPoint = row;
                    changed = true;
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (DragFloatInRange("##speed", &point.speedKmh, 0.0f, 400.0f, "%.0f", 0.5f))
                {
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    else if (path.points.empty())
    {
        ImGui::TextDisabled("No points: place them in the viewport, add them at the car, or record a line.");
    }
}

void DrivePathsPanel::DrawFollow(EditorContext& context, const std::vector<SceneDrivePath>& paths)
{
    const IEditorWorld& scene = context.scene;
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    EditorUiFrameResult& result = context.result;
    ImGui::SeparatorText("Follow");
    DragFloatInRange("Speed Scale", &m_followTrack.speedScale, 0.1f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Every speed the path sets times this.");
    }
    DragFloatInRange("Corner Grip Cap (g)", &m_followTrack.lateralGrip, 0.0f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Above 0, the speed in a bend is held to what this much lateral acceleration allows.\n0 drives the speeds as set, to find the car's limit.");
    }
    DragFloatInRange("Braking (g)", &m_followTrack.brakingDecel, 0.1f, 1.5f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The deceleration the speed plan brakes at before a slower stretch.");
    }

    const bool pathReady = m_selectedPath >= 0 && paths[static_cast<size_t>(m_selectedPath)].points.size() >= 2;
    const bool carReady = status.active || (scene.HasSelection() && scene.HasModelComponent(scene.GetSelectedEntity()));
    ImGui::BeginDisabled(!pathReady || !carReady);
    if (ImGui::Button(ICON_PH_STEERING_WHEEL " Follow Path") && pathReady)
    {
        result.actions.followDrivePath = EditorUiActions::DrivePathFollow{paths[static_cast<size_t>(m_selectedPath)].name, m_followTrack};
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "The car is put on the path's start and drives it by itself: pure pursuit steering, the path's speeds.\n"
            "Without a car being driven, the selected model is driven first. Reset (Backspace) starts it again.");
    }
    if (status.automation.mode != VehicleAutomationMode::None)
    {
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_STOP " Take Over"))
        {
            result.actions.stopDriveAutomation = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Stops the path or replay; the keyboard and gamepad drive the car again.");
        }
        const ImVec4 color = status.automation.status == PathFollowerStatus::Failed ? ui_colors::kTextDanger : ui_colors::kTextAccent;
        ImGui::TextColored(color, "%s", DescribeAutomation(status.automation).c_str());
    }
    if (!pathReady && m_selectedPath >= 0)
    {
        ImGui::TextDisabled("The path needs two points or more.");
    }
    else if (!carReady)
    {
        ImGui::TextDisabled("Select the car's model, or drive it.");
    }
    if (!status.lastRunSummary.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui_colors::kTextSecondary, "Last run: %s", status.lastRunSummary.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!status.lastError.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui_colors::kTextDanger, "%s", status.lastError.c_str());
        ImGui::PopTextWrapPos();
    }
}

void DrivePathsPanel::DrawRecording(EditorContext& context, std::vector<SceneDrivePath>& paths, bool& changed)
{
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    ImGui::SeparatorText("Record a Line");
    if (!m_recording)
    {
        ImGui::BeginDisabled(!status.active);
        if (ImGui::Button(ICON_PH_RECORD " Record Driven Line"))
        {
            m_recording = true;
            m_recorded.clear();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Drive the line by hand: a point every 5 m with the speed there, saved as a new path.");
        }
        return;
    }
    ImGui::TextColored(ui_colors::kTextWarning, ICON_PH_RECORD " Recording: %zu points", m_recorded.size());
    ImGui::BeginDisabled(m_recorded.size() < 2);
    if (ImGui::Button(ICON_PH_FLOPPY_DISK " Save as Path"))
    {
        SceneDrivePath path;
        path.name = FreeName(paths, "recorded");
        path.points = m_recorded;
        paths.push_back(std::move(path));
        m_selectedPath = static_cast<int>(paths.size()) - 1;
        m_selectedPoint = -1;
        m_recording = false;
        m_recorded.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_PH_X " Discard"))
    {
        m_recording = false;
        m_recorded.clear();
    }
}

void DrivePathsPanel::DrawDriveLog(EditorContext& context)
{
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    EditorUiFrameResult& result = context.result;
    ImGui::SeparatorText("Drive Log");
    const bool logging = !status.automation.logPath.empty();
    if (m_wasLogging && !logging)
    {
        m_logsScanned = false; // a log was just finished
    }
    m_wasLogging = logging;
    if (!logging)
    {
        ImGui::BeginDisabled(!status.active);
        if (ImGui::Button(ICON_PH_RECORD " Write Drive Log"))
        {
            result.actions.driveLog = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "Puts the car back at its start and writes every frame down (captures/drive_*.csv):\n"
                "the body's position and rotation, speed, controls, g, slip, each wheel's pose, load, travel and slip, and the\n"
                "path's error while following one. Replay plays it back frame by frame, exactly where it went.");
        }
    }
    else
    {
        if (ImGui::Button(ICON_PH_STOP " Stop Writing"))
        {
            result.actions.driveLog = false;
        }
        ImGui::SameLine();
        ImGui::TextColored(ui_colors::kTextWarning, "%s", std::filesystem::path(status.automation.logPath).filename().string().c_str());
    }

    if (!m_logsScanned)
    {
        ScanDriveLogs();
    }
    if (ImGui::SmallButton(ICON_PH_ARROW_CLOCKWISE " Refresh"))
    {
        ScanDriveLogs();
    }
    if (m_logs.empty())
    {
        ImGui::TextDisabled("No drive logs in captures yet.");
        return;
    }
    const bool canReplay = status.active || (context.scene.HasSelection() && context.scene.HasModelComponent(context.scene.GetSelectedEntity()));
    for (size_t index = 0; index < m_logs.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginDisabled(!canReplay || logging);
        if (ImGui::SmallButton(ICON_PH_PLAY " Replay"))
        {
            result.actions.replayDriveLog = m_logs[index].string();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(m_logs[index].filename().string().c_str());
        ImGui::PopID();
    }
}

void DrivePathsPanel::ScanDriveLogs()
{
    m_logsScanned = true;
    m_logs.clear();
    std::error_code error;
    const std::filesystem::path folder = EnginePaths::ProjectRoot() / "captures";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, error))
    {
        const std::filesystem::path& path = entry.path();
        if (entry.is_regular_file(error) && path.extension() == ".csv" && path.filename().string().starts_with("drive_"))
        {
            found.emplace_back(entry.last_write_time(error), path);
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b)
              {
                  return a.first > b.first;
              });
    for (size_t index = 0; index < found.size() && index < kListedLogs; ++index)
    {
        m_logs.push_back(found[index].second);
    }
}

bool DrivePathsPanel::DrawViewportOverlay(EditorContext& context, ImDrawList& drawList, const ImVec2& origin, const ImVec2& size, bool hovered, float uiScale)
{
    const VehicleDriveStatus& status = context.state.vehicleStatus;
    // The driven line, a point every few metres, whatever the panel shows.
    if (m_recording && status.active)
    {
        const glm::dvec3 position = status.pose.position;
        const auto across = [](const glm::dvec3& a, const glm::dvec3& b)
        {
            return glm::length(glm::dvec2(a.x - b.x, a.z - b.z));
        };
        if (m_recorded.empty() || across(position, m_recorded.back().position) >= kRecordSpacingMetres)
        {
            const float kmh = std::abs(status.telemetry.forwardSpeed) * 3.6f;
            m_recorded.push_back(SceneDrivePathPoint{position, kmh >= 1.0f ? std::round(kmh) : 0.0f});
        }
    }
    const bool following = status.automation.mode == VehicleAutomationMode::Path;
    if ((!IsOpen() && !following) || size.x <= 0.0f || size.y <= 0.0f)
    {
        return false;
    }

    IEditorWorld& scene = context.scene;
    const glm::mat4 viewProjection = context.matrices.projection * context.matrices.view;
    const std::vector<SceneDrivePath>& paths = scene.GetDrivePaths();
    const float thickness = 2.0f * uiScale;
    std::vector<ImVec2> line;
    const auto flush = [&](ImU32 color)
    {
        if (line.size() >= 2)
        {
            drawList.AddPolyline(line.data(), static_cast<int>(line.size()), color, ImDrawFlags_None, thickness);
        }
        line.clear();
    };
    for (size_t index = 0; index < paths.size(); ++index)
    {
        const SceneDrivePath& path = paths[index];
        const bool selected = static_cast<int>(index) == m_selectedPath;
        const bool followed = following && path.name == status.automation.name;
        if (!IsOpen() && !followed)
        {
            continue;
        }
        const ImU32 color = selected || followed ? kSelectedPathColor : kPathColor;
        const DrivePathTrack& track = TrackFor(index, path);
        if (!track.Empty())
        {
            // About a sample a metre, and the closing stretch of a closed path.
            const size_t stride = std::max<size_t>(1, static_cast<size_t>(1.0 / (track.length / static_cast<double>(track.samples.size()))));
            std::vector<size_t> drawn;
            for (size_t sample = 0; sample < track.samples.size(); sample += stride)
            {
                drawn.push_back(sample);
            }
            if (drawn.back() + 1 != track.samples.size())
            {
                drawn.push_back(track.samples.size() - 1);
            }
            if (track.closed)
            {
                drawn.push_back(0);
            }
            for (const size_t sample : drawn)
            {
                ImVec2 screen;
                if (Project(viewProjection, origin, size, track.samples[sample].position, screen))
                {
                    line.push_back(screen);
                }
                else
                {
                    flush(color);
                }
            }
            flush(color);
            // The start, and the way it goes from there.
            ImVec2 start;
            ImVec2 ahead;
            const DrivePathSample& first = track.samples.front();
            if (Project(viewProjection, origin, size, first.position, start) &&
                Project(viewProjection, origin, size, first.position + glm::dvec3(first.tangent) * 3.0, ahead))
            {
                drawList.AddCircleFilled(start, 5.0f * uiScale, color);
                drawList.AddLine(start, ahead, color, 3.0f * uiScale);
                drawList.AddText(ImVec2(start.x + 8.0f * uiScale, start.y - 8.0f * uiScale), color, path.name.c_str());
            }
        }
        if (selected && IsOpen())
        {
            for (size_t point = 0; point < path.points.size(); ++point)
            {
                ImVec2 screen;
                if (Project(viewProjection, origin, size, path.points[point].position, screen))
                {
                    const bool pointSelected = static_cast<int>(point) == m_selectedPoint;
                    drawList.AddCircleFilled(screen, (pointSelected ? 6.0f : 4.0f) * uiScale, pointSelected ? kSelectedPathColor : kPointColor);
                    drawList.AddCircle(screen, (pointSelected ? 6.0f : 4.0f) * uiScale, IM_COL32(0, 0, 0, 200), 0, 1.0f * uiScale);
                }
            }
        }
    }
    // The recorded line so far.
    if (m_recording)
    {
        for (const SceneDrivePathPoint& point : m_recorded)
        {
            ImVec2 screen;
            if (Project(viewProjection, origin, size, point.position, screen))
            {
                line.push_back(screen);
            }
        }
        flush(IM_COL32(236, 126, 126, 255));
    }
    // Where the following car is on the path and the point it steers for.
    if (following)
    {
        ImVec2 closest;
        ImVec2 lookahead;
        if (Project(viewProjection, origin, size, status.automation.closest, closest) &&
            Project(viewProjection, origin, size, status.automation.lookahead, lookahead))
        {
            drawList.AddLine(closest, lookahead, kLookaheadColor, 1.5f * uiScale);
            drawList.AddCircle(lookahead, 6.0f * uiScale, kLookaheadColor, 0, 2.0f * uiScale);
        }
    }

    // Clicks: on a point of the selected path, select it; elsewhere, with placing on, add one.
    if (!IsOpen() || !hovered || m_selectedPath < 0 || m_selectedPath >= static_cast<int>(paths.size()) || (!m_editPoints && !m_placePoints) ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGuizmo::IsOver() || ImGuizmo::IsUsing())
    {
        return false;
    }
    std::vector<SceneDrivePath> edited = paths;
    SceneDrivePath& path = edited[static_cast<size_t>(m_selectedPath)];
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float nearest = kPointPickRadius * uiScale;
    int picked = -1;
    for (size_t point = 0; point < path.points.size(); ++point)
    {
        ImVec2 screen;
        if (Project(viewProjection, origin, size, path.points[point].position, screen))
        {
            const float distance = std::hypot(screen.x - mouse.x, screen.y - mouse.y);
            if (distance < nearest)
            {
                nearest = distance;
                picked = static_cast<int>(point);
            }
        }
    }
    if (picked >= 0)
    {
        m_selectedPoint = picked;
        return true;
    }
    if (!m_placePoints)
    {
        return false;
    }
    // Level with the selected point, else the last, else the car, else the ground at 0.
    double height = 0.0;
    if (m_selectedPoint >= 0 && m_selectedPoint < static_cast<int>(path.points.size()))
    {
        height = path.points[static_cast<size_t>(m_selectedPoint)].position.y;
    }
    else if (!path.points.empty())
    {
        height = path.points.back().position.y;
    }
    else if (status.active)
    {
        height = status.pose.position.y;
    }
    const std::optional<glm::dvec3> placed = MouseOnPlane(viewProjection, origin, size, height);
    if (!placed.has_value())
    {
        return true;
    }
    const size_t at = m_selectedPoint >= 0 && m_selectedPoint < static_cast<int>(path.points.size()) ? static_cast<size_t>(m_selectedPoint) + 1
                                                                                                     : path.points.size();
    path.points.insert(path.points.begin() + static_cast<std::ptrdiff_t>(at), SceneDrivePathPoint{*placed, 0.0f});
    m_selectedPoint = static_cast<int>(at);
    scene.SetDrivePaths(std::move(edited));
    return true;
}

bool DrivePathsPanel::DrawPointGizmo(EditorContext& context, ImDrawList& drawList, const ImVec2& origin, const ImVec2& size, float uiScale)
{
    IEditorWorld& scene = context.scene;
    const std::vector<SceneDrivePath>& paths = scene.GetDrivePaths();
    if (!IsOpen() || !m_editPoints || m_selectedPath < 0 || m_selectedPath >= static_cast<int>(paths.size()) || m_selectedPoint < 0 ||
        m_selectedPoint >= static_cast<int>(paths[static_cast<size_t>(m_selectedPath)].points.size()))
    {
        return false;
    }
    if (size.x <= 0.0f || size.y <= 0.0f)
    {
        return true;
    }
    const glm::dvec3 position = paths[static_cast<size_t>(m_selectedPath)].points[static_cast<size_t>(m_selectedPoint)].position;
    glm::mat4 matrix = glm::translate(glm::mat4(1.0f), glm::vec3(position));
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetID(0x44500000 + m_selectedPath * 4096 + m_selectedPoint);
    ImGuizmo::SetDrawlist(&drawList);
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::Manipulate(
        glm::value_ptr(context.matrices.view), glm::value_ptr(context.matrices.projection), ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(matrix));
    (void)uiScale;
    if (ImGuizmo::IsUsing())
    {
        std::vector<SceneDrivePath> edited = paths;
        edited[static_cast<size_t>(m_selectedPath)].points[static_cast<size_t>(m_selectedPoint)].position = glm::dvec3(glm::vec3(matrix[3]));
        scene.SetDrivePaths(std::move(edited));
    }
    return true;
}
}
