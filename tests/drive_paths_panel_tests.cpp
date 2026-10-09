// The Drive Paths panel drawn headless: its paths and points, a car following one and how a run
// ended, and the viewport overlay with a click that places a point. With MINIENGINE_UI_SNAPSHOT_DIR
// set each is written there as a PNG to look at.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/drive_paths_panel.h>
#include <engine/logic/editor_scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "imgui_software_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr int kWidth = 560;
constexpr int kHeight = 900;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

SceneDrivePath LaneChange()
{
    SceneDrivePath path;
    path.name = "lane_change";
    path.speedKmh = 60.0f;
    path.points = {{{-60.0, 0.0, 0.0}}, {{-10.0, 0.0, 0.0}}, {{15.0, 0.0, -3.5}}, {{60.0, 0.0, -3.5}, 40.0f}};
    return path;
}

SceneDrivePath Circle()
{
    SceneDrivePath path;
    path.name = "circle_r30";
    path.closed = true;
    path.laps = 2;
    path.speedKmh = 50.0f;
    for (int index = 0; index < 12; ++index)
    {
        const double angle = 2.0 * 3.14159265358979 * index / 12.0;
        path.points.push_back({{30.0 * std::cos(angle), 0.0, 40.0 + 30.0 * std::sin(angle)}});
    }
    return path;
}

struct Fixture
{
    EditorScene scene;
    Camera camera;
    ViewportMatrices matrices;
    EditorFrameInput frame;
    EditorUiFrameResult result;
    EditorSharedState state;
    EditorStyle style;
    EditorWindowManager windows;
    CommandRegistry commands;
    DrivePathsPanel panel;

    Fixture()
    {
        scene.CreateEmptyScene();
        scene.SetDrivePaths({LaneChange(), Circle()});
        panel.Open();
        // Looking down on the paths from above and behind.
        matrices.view = glm::lookAt(glm::vec3(0.0f, 70.0f, -70.0f), glm::vec3(0.0f, 0.0f, 15.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        matrices.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.1f, 1000.0f);
    }

    // Two frames, so the window has its size; the second is the one looked at.
    void Draw(const char* snapshot)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            result = EditorUiFrameResult{};
            EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)));
            panel.Draw(context);
            ImGui::Render();
            test::ServeTextures(*ImGui::GetDrawData());
        }
        Require(!ImGui::GetDrawData()->CmdLists.empty(), std::string("the panel draws for ") + snapshot);
        Write(snapshot);
    }

    // The viewport overlay over the whole screen, with the mouse at `mouse` and clicked when `click`;
    // returns whether the overlay took the click.
    bool DrawOverlay(const char* snapshot, ImVec2 mouse, bool click)
    {
        bool taken = false;
        for (int pass = 0; pass < 3; ++pass)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(mouse.x, mouse.y);
            // Down in the second frame only: ImGui sees the press there.
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, click && pass == 1);
            result = EditorUiFrameResult{};
            EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
            ImGui::NewFrame();
            ImDrawList* drawList = ImGui::GetBackgroundDrawList();
            drawList->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)), IM_COL32(40, 44, 48, 255));
            taken |= panel.DrawViewportOverlay(
                context, *drawList, ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)), true, 1.0f);
            ImGui::Render();
            test::ServeTextures(*ImGui::GetDrawData());
        }
        Write(snapshot);
        return taken;
    }

    void Write(const char* snapshot)
    {
        if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
        {
            test::WritePng(test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / snapshot);
        }
    }
};

void TestPanel()
{
    Fixture fixture;
    fixture.Draw("drive_paths_idle.png");

    // A car following the circle, on its second lap.
    fixture.state.vehicleStatus.active = true;
    fixture.state.vehicleStatus.vehicleName = "skyline_r34_vspec";
    VehicleAutomationStatus& automation = fixture.state.vehicleStatus.automation;
    automation.mode = VehicleAutomationMode::Path;
    automation.name = "circle_r30";
    automation.distance = 120.0;
    automation.length = 188.4;
    automation.lap = 1;
    automation.laps = 2;
    automation.lateralError = 0.21f;
    automation.targetKmh = 50.0f;
    automation.closest = glm::dvec3(30.0, 0.0, 40.0);
    automation.lookahead = glm::dvec3(26.0, 0.0, 55.0);
    automation.logPath = "captures/drive_20261009_120000.csv";
    fixture.state.vehicleStatus.lastRunSummary =
        "Path 'lane_change' finished: 15.50 s, 190 m, top 60.0 km/h, lateral 0.18 g, braking 1.02 g, accelerating 0.55 g, off the path 0.18 m RMS, 0.61 m at most";
    fixture.Draw("drive_paths_following.png");
    Require(!fixture.DrawOverlay("drive_paths_overlay.png", ImVec2(-100.0f, -100.0f), false), "the overlay takes no click without one");

    // Edit Points is on by default: a click on a point of the selected path (the first) selects it, and
    // the overlay takes the click from the scene's selection.
    const glm::vec4 clip = fixture.matrices.projection * fixture.matrices.view * glm::vec4(-10.0f, 0.0f, 0.0f, 1.0f);
    const ImVec2 onPoint((clip.x / clip.w * 0.5f + 0.5f) * kWidth, (0.5f - clip.y / clip.w * 0.5f) * kHeight);
    Require(fixture.DrawOverlay("drive_paths_overlay_click.png", onPoint, true), "a click on a point is the overlay's");
    Require(!fixture.DrawOverlay("drive_paths_overlay_miss.png", ImVec2(kWidth * 0.5f, kHeight * 0.95f), true), "a click away from the points, with placing off, is the scene's");
    Require(fixture.scene.GetDrivePaths()[0].points.size() == 4, "no point was added");
}
}

int main()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    int result = 0;
    try
    {
        TestPanel();
        std::cout << "drive_paths_panel_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "drive_paths_panel_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
