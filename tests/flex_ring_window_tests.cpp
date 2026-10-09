// The editor's Flex Ring Tyre panel without a GPU: ImGui and ImPlot run headless, the panel
// pre-processes the default tyre, rolls it on the live rig over cleats and draws every tab. With
// MINIENGINE_UI_SNAPSHOT_DIR set each tab is rasterised to a PNG there (and the quick test programs run
// for the Tests tab's picture).

#include "imgui_software_raster.h"

#include <engine/editor/ui/panels/flex_ring_tyre_panel.h>

#include <imgui.h>
#include <implot.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace me;

namespace
{
constexpr int kWidth = 1500;
constexpr int kHeight = 950;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void Frame(FlexRingTyrePanel& panel, const char* snapshot = nullptr)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(panel.GetTitle().c_str());
    panel.DrawContents(io.DeltaTime);
    ImGui::End();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    test::ServeTextures(*drawData);
    const char* folder = std::getenv("MINIENGINE_UI_SNAPSHOT_DIR");
    if (snapshot != nullptr && folder != nullptr)
    {
        std::filesystem::create_directories(folder);
        test::WritePng(test::Rasterise(*drawData, kWidth, kHeight), kWidth, kHeight, std::filesystem::path(folder) / snapshot);
    }
}

void Frames(FlexRingTyrePanel& panel, int count, const char* snapshot = nullptr)
{
    for (int i = 0; i < count; ++i)
    {
        if (std::getenv("MINIENGINE_TRACE_FRAMES") != nullptr)
        {
            std::cout << "frame " << i << " t " << panel.LiveTime() << std::endl;
        }
        Frame(panel, i + 1 == count ? snapshot : nullptr);
    }
}

void TestPanelRollsTheTyre()
{
    FlexRingTyrePanel panel;
    Frame(panel);
    Require(panel.IsPreprocessing(), "the panel pre-processes the default tyre by itself");
    panel.WaitForWork();
    Require(panel.HasModel(), "the model is ready");

    // Standing under 3 kN.
    panel.Live().mode = 0;
    Frames(panel, 30, "flexring_live_standing.png");
    Require(panel.LiveTime() > 0.2, "the live rig runs (" + std::to_string(panel.LiveTime()) + " s)");

    // Rolling at 60 km/h, 3 deg slip angle, over transversal cleats.
    panel.Live().mode = 1;
    panel.Live().slipAngleDeg = 3.0f;
    panel.Live().road = 1;
    Frames(panel, 90, "flexring_live_rolling.png");
    Require(panel.LiveTime() > 1.0, "the live rig keeps running");

    panel.RequestTab(FlexRingTyrePanel::ParametersTab);
    Frames(panel, 2, "flexring_parameters.png");
    panel.RequestTab(FlexRingTyrePanel::ModesTab);
    Frames(panel, 2, "flexring_modes.png");

    if (std::getenv("MINIENGINE_UI_SNAPSHOT_DIR") != nullptr)
    {
        panel.RequestTab(FlexRingTyrePanel::TestsTab);
        Frame(panel);
        panel.StartTests(true);
        panel.WaitForWork();
        Require(panel.HasTestResults(), "the test programs ran");
        Frames(panel, 2, "flexring_tests.png");
    }
}
}

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    int result = 0;
    try
    {
        TestPanelRollsTheTyre();
        std::cout << "flex ring window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "flex ring window tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return result;
}
