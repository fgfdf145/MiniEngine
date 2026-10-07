// The Graphics Debug panel drawn headless in each pipeline mode: hybrid, path tracing, ReSTIR PT,
// forward only and the Khronos reference view. Each greys out the controls the pipeline does not use
// (render_features.h); drawing every mode checks the panel's disabled scopes stay balanced, and with
// MINIENGINE_UI_SNAPSHOT_DIR set each mode is written there as a PNG to look at.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/graphics_debug_panel.h>
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
constexpr int kWidth = 460;
constexpr int kHeight = 2000;

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
    GraphicsDebugPanel panel;

    Fixture()
    {
        // A GPU with ray queries and DLSS, as the backend reports them.
        state.pathTracingAvailable = true;
        state.dlssAvailable = true;
        state.dlssRayReconstructionAvailable = true;
        panel.Open();
    }

    // Two frames, so the window has its size; the second is the one written out.
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

void TestEveryMode()
{
    Fixture fixture;
    RenderDebugSettings& debug = fixture.state.renderDebug;
    fixture.Draw("graphics_debug_hybrid.png");

    debug.pathTracing.enabled = true;
    fixture.Draw("graphics_debug_path_tracing.png");

    debug.pathTracing.restir = true;
    fixture.Draw("graphics_debug_restir_pt.png");

    debug.dlssMode = DlssMode::Quality;
    debug.dlssRayReconstruction = true;
    fixture.Draw("graphics_debug_restir_pt_dlss.png");

    debug = RenderDebugSettings{};
    debug.forwardOnly = true;
    fixture.Draw("graphics_debug_forward_only.png");

    debug = RenderDebugSettings{};
    debug.khronosReference = true;
    fixture.Draw("graphics_debug_khronos.png");

    debug = RenderDebugSettings{};
    fixture.state.pathTracingAvailable = false;
    fixture.Draw("graphics_debug_no_ray_queries.png");
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
        TestEveryMode();
        std::cout << "graphics_debug_panel_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "graphics_debug_panel_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
