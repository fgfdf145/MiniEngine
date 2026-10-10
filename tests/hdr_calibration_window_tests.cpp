// Render > HDR Calibration drawn headless: Enter steps through the PS5-style screens, the arrow keys
// move the trial level and the pattern follows, Cancel (leaving fullscreen) puts everything back and
// Finish keeps the calibration. With MINIENGINE_UI_SNAPSHOT_DIR set each step is written there as a PNG.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/windows/hdr_calibration_window.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include "imgui_software_raster.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
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

    explicit Fixture(bool hdr)
    {
        windows.Register<HdrCalibrationWindow>();
        // What the renderer reports on the user's display.
        state.display.report.known = true;
        state.display.report.hdrEnabled = true;
        state.display.report.maxLuminance = 600.0f;
        state.display.report.maxFullFrameLuminance = 600.0f;
        state.display.report.minLuminance = 0.005f;
        state.display.report.sdrWhiteNits = 480.0f;
        state.display.report.name = "\\\\.\\DISPLAY1";
        state.display.hdrRequested = hdr;
        state.renderDebug.hdrOutput = hdr;
        state.display.output.hdr = hdr;
        state.display.output.maxLuminance = 600.0f;
        state.display.output.maxFullFrameLuminance = 600.0f;
        state.display.output.minLuminance = 0.005f;
        state.display.output.uiWhiteNits = 480.0f;
        state.display.output.paperWhiteNits = 480.0f;
    }

    // One frame, with key pressed through it when given; the snapshot, if any, is this frame.
    void Frame(ImGuiKey key = ImGuiKey_None, const char* snapshot = nullptr)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        if (key != ImGuiKey_None)
        {
            io.AddKeyEvent(key, true);
        }
        result = EditorUiFrameResult{};
        EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
        ImGui::NewFrame();
        windows.TickAndDraw(context, state.commands.viewportFullscreen);
        ImGui::Render();
        test::ServeTextures(*ImGui::GetDrawData());
        if (key != ImGuiKey_None)
        {
            io.AddKeyEvent(key, false);
        }
        if (snapshot != nullptr)
        {
            if (const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR"))
            {
                test::WritePng(test::Rasterise(*ImGui::GetDrawData(), kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / snapshot);
            }
        }
    }

    // Presses key, then lets a frame go by without it.
    void Press(ImGuiKey key, const char* snapshot = nullptr)
    {
        Frame(key);
        Frame(ImGuiKey_None, snapshot);
    }

    CalibrationPattern Pattern() const
    {
        return state.renderDebug.calibrationView.pattern;
    }
};

void TestStepsAndCancel()
{
    Fixture fixture(true);
    const DisplaySettings before = fixture.state.renderDebug.display;
    fixture.windows.Open<HdrCalibrationWindow>();
    fixture.Frame();
    fixture.Frame(ImGuiKey_None, "hdr_calibration_start.png");
    Require(fixture.state.commands.viewportFullscreen, "the calibration goes fullscreen");
    Require(!fixture.state.commands.viewportUi && !fixture.state.commands.gizmos, "the viewport's overlays are hidden");
    Require(fixture.Pattern() == CalibrationPattern::None, "the start shows the scene");

    fixture.Press(ImGuiKey_Enter, "hdr_calibration_full_frame.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrFullFrame, "Enter starts with the full-frame screen");
    Require(fixture.state.renderDebug.display.calibrated, "the luminances are now the calibration's");
    Require(fixture.state.renderDebug.display.maxFullFrameLuminance == 600.0f, "it starts from the display's own figure");
    Require(fixture.state.renderDebug.calibrationView.level == 600.0f, "the pattern shows the level");

    fixture.Press(ImGuiKey_RightArrow);
    const float raised = fixture.state.renderDebug.display.maxFullFrameLuminance;
    Require(raised > 600.0f && raised < 700.0f, "the right arrow raises the level a notch, got " + std::to_string(raised));
    Require(fixture.state.renderDebug.calibrationView.level == raised, "the pattern follows the level");

    fixture.Press(ImGuiKey_Enter, "hdr_calibration_window.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrWindow, "then the 10 % window");
    fixture.Press(ImGuiKey_Enter, "hdr_calibration_black.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrBlack, "then black");
    fixture.Press(ImGuiKey_LeftArrow);
    Require(fixture.state.renderDebug.display.minLuminance < 0.005f, "the left arrow lowers the black level");
    fixture.Press(ImGuiKey_Enter, "hdr_calibration_gt_peak.png");
    Require(fixture.Pattern() == CalibrationPattern::GtPeak, "then Gran Turismo's checkerboard");
    Require(fixture.state.renderDebug.display.scenePeakNits == 600.0f, "the scene's peak starts from the 10 % window's");
    fixture.Press(ImGuiKey_RightArrow);
    Require(fixture.state.renderDebug.display.scenePeakNits > 600.0f, "the right arrow raises the scene's peak");
    Require(fixture.state.renderDebug.display.maxLuminance == 600.0f, "without touching the display's");
    fixture.Press(ImGuiKey_Backspace);
    fixture.Press(ImGuiKey_Backspace);
    Require(fixture.Pattern() == CalibrationPattern::HdrWindow, "Backspace goes back a step");

    // Escape leaves the fullscreen viewport (the editor shell does that): Cancel.
    fixture.state.commands.viewportFullscreen = false;
    fixture.Frame();
    fixture.Frame();
    Require(!fixture.windows.Get<HdrCalibrationWindow>().IsOpen(), "leaving fullscreen closes the calibration");
    Require(fixture.state.renderDebug.display == before, "Cancel puts the old settings back");
    Require(fixture.Pattern() == CalibrationPattern::None, "no pattern after it");
    Require(!fixture.state.commands.viewportFullscreen && fixture.state.commands.viewportUi && fixture.state.commands.gizmos, "the viewport is as it was");
}

void TestFinishKeepsTheCalibration()
{
    Fixture fixture(true);
    fixture.windows.Open<HdrCalibrationWindow>();
    fixture.Frame();
    fixture.Frame();
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_LeftArrow);
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_RightArrow);
    fixture.Press(ImGuiKey_Enter);
    // The window sizes itself to the new step a frame later.
    fixture.Frame(ImGuiKey_None, "hdr_calibration_review.png");
    Require(fixture.Pattern() == CalibrationPattern::None, "the review shows the scene");
    fixture.Press(ImGuiKey_Enter);
    fixture.Frame();
    Require(!fixture.windows.Get<HdrCalibrationWindow>().IsOpen(), "Finish closes the calibration");
    const DisplaySettings& kept = fixture.state.renderDebug.display;
    Require(kept.calibrated && kept.maxFullFrameLuminance < 600.0f, "Finish keeps the calibration");
    Require(kept.maxLuminance == 600.0f && kept.minLuminance == 0.005f, "untouched levels keep their starting values");
    Require(kept.scenePeakNits > 600.0f, "Finish keeps the scene's peak");
    Require(fixture.Pattern() == CalibrationPattern::None && !fixture.state.commands.viewportFullscreen, "back to the editor");
}

void TestSdrCannotStart()
{
    Fixture fixture(false);
    fixture.windows.Open<HdrCalibrationWindow>();
    fixture.Frame();
    fixture.Frame(ImGuiKey_None, "hdr_calibration_sdr.png");
    fixture.Press(ImGuiKey_Enter);
    Require(fixture.Pattern() == CalibrationPattern::None, "on an SDR swapchain Enter does not start the patterns");
    Require(!fixture.state.renderDebug.display.calibrated, "nothing is calibrated");
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
        TestStepsAndCancel();
        TestFinishKeepsTheCalibration();
        TestSdrCannotStart();
        std::cout << "hdr_calibration_window_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "hdr_calibration_window_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
