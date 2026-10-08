// View > Display Calibration drawn headless: the HDR and SDR paths step by Enter, the arrow keys move
// the trial level and the pattern follows, Cancel (leaving fullscreen) puts everything back and Finish
// keeps the calibration. With MINIENGINE_UI_SNAPSHOT_DIR set each step is written there as a PNG.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/windows/display_calibration_window.h>
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
        windows.Register<DisplayCalibrationWindow>();
        // What the renderer reports on the user's display.
        state.display.report.known = true;
        state.display.report.hdrEnabled = true;
        state.display.report.maxLuminance = 600.0f;
        state.display.report.maxFullFrameLuminance = 600.0f;
        state.display.report.minLuminance = 0.005f;
        state.display.report.sdrWhiteNits = 480.0f;
        state.display.report.name = "\\\\.\\DISPLAY1";
        state.display.hdrRequested = hdr;
        state.display.hdrActive = hdr;
        state.display.output.hdr = hdr;
        state.display.output.maxLuminance = 600.0f;
        state.display.output.maxFullFrameLuminance = 600.0f;
        state.display.output.minLuminance = 0.005f;
        state.display.output.uiWhiteNits = 480.0f;
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

void TestHdrPathAndCancel()
{
    Fixture fixture(true);
    const DisplaySettings before = fixture.state.renderDebug.display;
    fixture.windows.Open<DisplayCalibrationWindow>();
    fixture.Frame();
    fixture.Frame(ImGuiKey_None, "calibration_hdr_start.png");
    Require(fixture.state.commands.viewportFullscreen, "the calibration goes fullscreen");
    Require(!fixture.state.commands.viewportUi && !fixture.state.commands.gizmos, "the viewport's overlays are hidden");
    Require(fixture.Pattern() == CalibrationPattern::None, "the start shows the scene");

    fixture.Press(ImGuiKey_Enter, "calibration_hdr_full_frame.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrFullFrame, "Enter starts with the full-frame screen");
    Require(fixture.state.renderDebug.display.calibrated, "the luminances are now the calibration's");
    Require(fixture.state.renderDebug.display.maxFullFrameLuminance == 600.0f, "it starts from the display's own figure");
    Require(fixture.state.renderDebug.calibrationView.level == 600.0f, "the pattern shows the level");

    fixture.Press(ImGuiKey_RightArrow);
    const float raised = fixture.state.renderDebug.display.maxFullFrameLuminance;
    Require(raised > 600.0f && raised < 700.0f, "the right arrow raises the level a notch, got " + std::to_string(raised));
    Require(fixture.state.renderDebug.calibrationView.level == raised, "the pattern follows the level");

    fixture.Press(ImGuiKey_Enter, "calibration_hdr_window.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrWindow, "then the 10 % window");
    fixture.Press(ImGuiKey_Enter, "calibration_hdr_black.png");
    Require(fixture.Pattern() == CalibrationPattern::HdrBlack, "then black");
    fixture.Press(ImGuiKey_LeftArrow);
    Require(fixture.state.renderDebug.display.minLuminance < 0.005f, "the left arrow lowers the black level");
    fixture.Press(ImGuiKey_Backspace);
    Require(fixture.Pattern() == CalibrationPattern::HdrWindow, "Backspace goes back a step");

    // Escape leaves the fullscreen viewport (the editor shell does that): Cancel.
    fixture.state.commands.viewportFullscreen = false;
    fixture.Frame();
    fixture.Frame();
    Require(!fixture.windows.Get<DisplayCalibrationWindow>().IsOpen(), "leaving fullscreen closes the calibration");
    Require(fixture.state.renderDebug.display == before, "Cancel puts the old settings back");
    Require(fixture.Pattern() == CalibrationPattern::None, "no pattern after it");
    Require(!fixture.state.commands.viewportFullscreen && fixture.state.commands.viewportUi && fixture.state.commands.gizmos, "the viewport is as it was");
}

void TestHdrPathFinishes()
{
    Fixture fixture(true);
    fixture.windows.Open<DisplayCalibrationWindow>();
    fixture.Frame();
    fixture.Frame();
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_LeftArrow);
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_Enter);
    fixture.Press(ImGuiKey_Enter, "calibration_hdr_picture.png");
    Require(fixture.Pattern() == CalibrationPattern::None, "the picture step starts on the scene");
    fixture.Press(ImGuiKey_E);
    Require(fixture.Pattern() == CalibrationPattern::SampleWedge, "E switches to the step wedge");
    fixture.Press(ImGuiKey_E, "calibration_hdr_sky.png");
    Require(fixture.Pattern() == CalibrationPattern::SampleSky, "then the sky");
    fixture.Press(ImGuiKey_Q);
    Require(fixture.Pattern() == CalibrationPattern::SampleWedge, "Q switches back");
    fixture.Press(ImGuiKey_Enter, "calibration_hdr_review.png");
    fixture.Press(ImGuiKey_Enter);
    fixture.Frame();
    Require(!fixture.windows.Get<DisplayCalibrationWindow>().IsOpen(), "Finish closes the calibration");
    const DisplaySettings& kept = fixture.state.renderDebug.display;
    Require(kept.calibrated && kept.maxFullFrameLuminance < 600.0f, "Finish keeps the calibration");
    Require(kept.maxLuminance == 600.0f && kept.minLuminance == 0.005f, "untouched levels keep their starting values");
    Require(fixture.Pattern() == CalibrationPattern::None && !fixture.state.commands.viewportFullscreen, "back to the editor");
}

void TestSdrPath()
{
    Fixture fixture(false);
    fixture.windows.Open<DisplayCalibrationWindow>();
    fixture.Frame();
    fixture.Frame(ImGuiKey_None, "calibration_sdr_start.png");
    fixture.Press(ImGuiKey_Enter, "calibration_sdr_bright.png");
    Require(fixture.Pattern() == CalibrationPattern::SdrBright, "SDR starts with the bright section");
    fixture.Press(ImGuiKey_LeftArrow);
    Require(fixture.state.renderDebug.display.sdrWhite < 1.0f, "the left arrow lowers the white signal");
    fixture.Press(ImGuiKey_Enter, "calibration_sdr_dark.png");
    Require(fixture.Pattern() == CalibrationPattern::SdrDark, "then the dark section");
    fixture.Press(ImGuiKey_RightArrow);
    Require(fixture.state.renderDebug.display.sdrBlack > 0.0f, "the right arrow raises the black signal");
    fixture.Press(ImGuiKey_Enter);
    Require(fixture.state.renderDebug.calibrationView.pattern == CalibrationPattern::None, "then the picture");
    Require(!fixture.state.renderDebug.display.calibrated, "SDR leaves the HDR luminances to the display");
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
        TestHdrPathAndCancel();
        TestHdrPathFinishes();
        TestSdrPath();
        std::cout << "display_calibration_window_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "display_calibration_window_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
