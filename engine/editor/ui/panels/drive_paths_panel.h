#pragma once

#include <engine/editor/services/vehicle_path_follower.h>
#include <engine/editor/ui/framework/editor_panel.h>
#include <engine/scene/scene_drive_path.h>

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace me
{

// Lines on the map for a car to follow by itself in driving tests
// (docs/design/2026-10-09-drive-path-follow-design.md): the scene's drive paths and their points,
// placed in the viewport, moved with the gizmo or recorded from a drive; following one; and writing a
// drive down and replaying it.
class DrivePathsPanel final : public EditorPanel
{
  public:
    DrivePathsPanel();

    // While the panel is open (or a car follows a path): draws the scene's drive paths over the viewport
    // (origin and size in screen points), and where a following car steers for; selects and places
    // points with the clicks. Records the driven line while that is on. True when it took a click, which
    // then selects nothing in the scene.
    bool DrawViewportOverlay(EditorContext& context, ImDrawList& drawList, const ImVec2& origin, const ImVec2& size, bool hovered, float uiScale);
    // The move gizmo on the selected point while points are edited. True when it is drawn, in place of
    // the selected entity's.
    bool DrawPointGizmo(EditorContext& context, ImDrawList& drawList, const ImVec2& origin, const ImVec2& size, float uiScale);

  protected:
    void OnGui(EditorContext& context) override;

  private:
    struct CachedTrack
    {
        SceneDrivePath path;
        DrivePathTrack track;
    };
    // The path sampled as the follower sees it, built again only when the path changes.
    const DrivePathTrack& TrackFor(size_t index, const SceneDrivePath& path);
    void DrawPathList(std::vector<SceneDrivePath>& paths, bool& changed);
    void DrawPathEditor(EditorContext& context, std::vector<SceneDrivePath>& paths, bool& changed);
    void DrawConnect(std::vector<SceneDrivePath>& paths, bool& changed);
    void DrawFollow(EditorContext& context, const std::vector<SceneDrivePath>& paths);
    void DrawRecording(EditorContext& context, std::vector<SceneDrivePath>& paths, bool& changed);
    void DrawDriveLog(EditorContext& context);
    void ScanDriveLogs();

    std::vector<CachedTrack> m_tracks;
    int m_selectedPath = -1;
    int m_selectedPoint = -1;
    // The gizmo moves the selected point and a click on a point selects it; with placing on, a click
    // elsewhere adds a point after the selected one.
    bool m_editPoints = true;
    bool m_placePoints = false;
    DrivePathTrackSettings m_followTrack;
    // Connecting the selected path's end to another path's end (by index, -1 none chosen yet).
    DrivePathEnd m_linkFromEnd = DrivePathEnd::End;
    int m_linkTo = -1;
    DrivePathEnd m_linkToEnd = DrivePathEnd::Start;
    bool m_linkJoin = false;
    // The link as it would be added, drawn over the viewport.
    std::vector<SceneDrivePathPoint> m_linkPreview;
    // The driven line being recorded: a point every few metres with the speed there.
    bool m_recording = false;
    std::vector<SceneDrivePathPoint> m_recorded;
    // Drive logs under the project's captures folder, newest first.
    std::vector<std::filesystem::path> m_logs;
    bool m_logsScanned = false;
    bool m_wasLogging = false;
};
}
