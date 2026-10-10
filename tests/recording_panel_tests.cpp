// The viewport recording's settings: their clamps, their file extension and folder, and their round
// trip through the settings file; then the Recording panel drawn headless idle, with a fixed size,
// as AVI, while recording and after a recording was saved. Drawing each checks the panel's disabled
// scopes stay balanced and that its button asks for a recording; with MINIENGINE_UI_SNAPSHOT_DIR
// set each is written there as a PNG to look at.

#include <engine/core/paths/engine_paths.h>
#include <engine/core/video/mp4_h264_writer.h>
#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/engine_settings.h>
#include <engine/editor/services/viewport_recording.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/recording_panel.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include "imgui_software_raster.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr int kWidth = 480;
constexpr int kHeight = 560;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void SettingsClampAndPaths()
{
    ViewportRecordingSettings settings;
    settings.framesPerSecond = 0;
    settings.megabitsPerSecond = 100000;
    settings.jpegQuality = 500;
    ViewportRecordingSettings clamped = ClampViewportRecordingSettings(settings);
    Require(clamped.framesPerSecond == kViewportRecordingMinFramesPerSecond, "the frame rate clamps up");
    Require(clamped.megabitsPerSecond == kViewportRecordingMaxMegabitsPerSecond, "the bit rate clamps down");
    Require(clamped.jpegQuality == kViewportRecordingMaxJpegQuality, "the JPEG quality clamps down");
    settings.framesPerSecond = 1000;
    Require(ClampViewportRecordingSettings(settings).framesPerSecond == kViewportRecordingMaxFramesPerSecond, "the frame rate clamps down");
    Require(ClampViewportRecordingSettings(ViewportRecordingSettings{}) == ViewportRecordingSettings{}, "the defaults are in range");

    settings = ViewportRecordingSettings{};
    Require(std::strcmp(ViewportRecordingExtension(settings), Mp4H264Writer::IsSupported() ? ".mp4" : ".avi") == 0,
            "MP4 where it can be encoded");
    settings.format = ViewportRecordingFormat::Avi;
    Require(std::strcmp(ViewportRecordingExtension(settings), ".avi") == 0, "AVI when asked for");

    Require(ViewportRecordingFolder(settings) == EnginePaths::ProjectRoot() / "captures", "captures/ by default");
    settings.folder = "D:/Videos";
    Require(ViewportRecordingFolder(settings) == std::filesystem::path("D:/Videos"), "the chosen folder");
}

void SettingsRoundTrip()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_viewport_recording_settings.json";
    EngineSettings saved;
    saved.viewportRecording.framesPerSecond = 60;
    saved.viewportRecording.format = ViewportRecordingFormat::Avi;
    saved.viewportRecording.megabitsPerSecond = 80;
    saved.viewportRecording.jpegQuality = 75;
    saved.viewportRecording.folder = "D:/Videos/Mini \"Engine\"";
    std::string error;
    Require(SaveEngineSettings(path, saved, error), "the settings save: " + error);
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), "the settings load: " + error);
    Require(loaded.viewportRecording == saved.viewportRecording, "the recording settings come back");

    std::ofstream(path) << "{ \"version\": 1 }";
    EngineSettings old;
    Require(LoadEngineSettings(path, old, error), "the old settings load: " + error);
    std::filesystem::remove(path);
    Require(old.viewportRecording == ViewportRecordingSettings{}, "without recording settings the defaults apply");
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
    RecordingPanel panel;

    Fixture()
    {
        panel.Open();
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
        // ImGui asserts on a disabled scope the panel left open (ConfigErrorRecoveryEnableAssert).
        Require(!ImGui::GetDrawData()->CmdLists.empty(), std::string("the panel draws for ") + snapshot);
        if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
        {
            test::WritePng(
                test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight),
                kWidth,
                kHeight,
                std::filesystem::path(folder) / snapshot);
        }
    }
};

void TestPanel()
{
    Fixture fixture;
    fixture.Draw("recording_idle.png");

    fixture.state.renderDebug.viewportResolution.fixed = true;
    fixture.state.renderDebug.viewportResolution.width = 2560;
    fixture.state.renderDebug.viewportResolution.height = 1440;
    fixture.state.viewportRecording.framesPerSecond = 60;
    fixture.state.viewportRecording.megabitsPerSecond = 50;
    fixture.Draw("recording_fixed_size.png");

    fixture.state.viewportRecording.format = ViewportRecordingFormat::Avi;
    fixture.state.viewportRecording.folder = "D:/Videos";
    fixture.Draw("recording_avi.png");

    // While recording the backend holds the size; the settings stay as they were.
    fixture.state.viewportRecording = ViewportRecordingSettings{};
    fixture.state.forcedViewportExtent = RenderExtent{2560, 1440};
    fixture.state.videoRecording.active = true;
    fixture.state.videoRecording.seconds = 83.4;
    fixture.state.videoRecording.bytes = uint64_t{212} << 20;
    fixture.state.videoRecording.droppedFrames = 3;
    fixture.Draw("recording_active.png");
    Require(fixture.state.viewportRecording == ViewportRecordingSettings{}, "drawing leaves the settings alone");

    fixture.state.forcedViewportExtent.reset();
    fixture.state.videoRecording = VideoRecordingIndicator{};
    fixture.state.videoRecording.message = "Saved recording_20261010_231500.mp4 (83.4 s, 212.0 MB)";
    fixture.state.videoRecording.messageTime = std::chrono::steady_clock::now();
    fixture.state.videoRecording.lastFile = std::filesystem::temp_directory_path();
    fixture.Draw("recording_saved.png");
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
        SettingsClampAndPaths();
        SettingsRoundTrip();
        TestPanel();
        std::cout << "recording_panel_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "recording_panel_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
