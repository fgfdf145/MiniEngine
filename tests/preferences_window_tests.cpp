// The Preferences window drawn headless with each CPU selection; Custom lays out a checkbox for every
// CPU of this machine, grouped by performance and efficiency cores on a hybrid CPU. With
// MINIENGINE_UI_SNAPSHOT_DIR set each one is written there as a PNG to look at.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/windows/preferences_window.h>
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
    PreferencesWindow window;

    Fixture()
    {
        state.audioStatus = "Speakers, 48000 Hz";
        state.processStatus = "High priority, CPUs 0-23 (24 of 24)";
        window.Open();
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
            window.Draw(context);
            ImGui::Render();
            test::ServeTextures(*ImGui::GetDrawData());
        }
        Require(!ImGui::GetDrawData()->CmdLists.empty(), std::string("the window draws for ") + snapshot);
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

void TestEverySelection()
{
    Fixture fixture;
    platform::process::ProcessAllocation& process = fixture.state.process;
    fixture.Draw("preferences_all_cpus.png");

    process.cpus = platform::process::CpuSelection::Performance;
    fixture.Draw("preferences_performance_cpus.png");

    process.priority = platform::process::ProcessPriority::Normal;
    process.cpus = platform::process::CpuSelection::Custom;
    process.customCpus = {0, 1, 2, 3, 8, 9};
    fixture.Draw("preferences_custom_cpus.png");

    // The last CPU cannot be taken away; drawing it greyed out must keep the disabled scopes balanced.
    process.customCpus = {5};
    fixture.Draw("preferences_one_cpu.png");
    Require(process.customCpus.size() == 1, "drawing alone changes no CPU");
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
        TestEverySelection();
        std::cout << "preferences_window_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "preferences_window_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
