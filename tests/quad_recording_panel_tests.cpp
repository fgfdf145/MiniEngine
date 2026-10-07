// The Quad Recording panel drawn headless: with a car to follow, with none, while recording, and in
// the cross layout. Drawing each checks the panel's disabled scopes stay balanced and that it asks for
// its preview; with MINIENGINE_UI_SNAPSHOT_DIR set each is written there as a PNG to look at, the
// cameras' pictures stood in for by a test card.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/imgui_frame_snapshot.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/quad_recording_panel.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include "imgui_software_raster.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr int kWidth = 520;
constexpr int kHeight = 1100;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

// A picture for the preview's tiles: a gradient, as the render thread would put a camera's there.
ImTextureData& TestCard()
{
    static std::unique_ptr<ImTextureData> card;
    if (!card)
    {
        card = std::make_unique<ImTextureData>();
        card->Create(ImTextureFormat_RGBA32, 32, 18);
        for (int y = 0; y < card->Height; ++y)
        {
            for (int x = 0; x < card->Width; ++x)
            {
                unsigned char* texel = static_cast<unsigned char*>(card->GetPixelsAt(x, y));
                texel[0] = static_cast<unsigned char>(40 + x * 6);
                texel[1] = static_cast<unsigned char>(60 + y * 8);
                texel[2] = 180;
                texel[3] = 255;
            }
        }
        card->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(card.get())));
        card->SetStatus(ImTextureStatus_OK);
    }
    return *card;
}

// What the render thread does to the preview's tiles: here, the test card in each.
size_t ServeCameraPictures(ImDrawData& drawData)
{
    size_t served = 0;
    for (ImDrawList* list : drawData.CmdLists)
    {
        for (ImDrawCmd& command : list->CmdBuffer)
        {
            if (std::find(kCaptureViewTextureIds.begin(), kCaptureViewTextureIds.end(), command.TexRef._TexID) != kCaptureViewTextureIds.end())
            {
                command.TexRef = ImTextureRef(TestCard().GetTexID());
                ++served;
            }
        }
    }
    return served;
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
    QuadRecordingPanel panel;

    Fixture()
    {
        state.quadRecordingTarget = "skyline_r34_vspec";
        panel.Open();
    }

    // Two frames, so the window has its size; the second is the one looked at. Returns how many of
    // the cameras' pictures it drew.
    size_t Draw(const char* snapshot)
    {
        size_t pictures = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            result = EditorUiFrameResult{};
            state.quadRecordingPreview = false;
            EditorContext context{scene, camera, matrices, frame, result, state, style, windows, commands};
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)));
            panel.Draw(context);
            ImGui::Render();
            test::ServeTextures(*ImGui::GetDrawData());
            pictures = ServeCameraPictures(*ImGui::GetDrawData());
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
        return pictures;
    }
};

void TestPanel()
{
    Fixture fixture;
    Require(fixture.Draw("quad_recording_idle.png") == 4, "the preview shows the four cameras");
    Require(fixture.state.quadRecordingPreview, "the panel asks for its preview while it shows");

    fixture.state.quadRecordingTarget.clear();
    Require(fixture.Draw("quad_recording_no_target.png") == 0, "nothing to film, no pictures");

    fixture.state.quadRecordingTarget = "skyline_r34_vspec";
    fixture.state.quadRecordingStatus.active = true;
    fixture.state.quadRecordingStatus.seconds = 75.0;
    fixture.state.quadRecordingStatus.bytes = 48u * 1024u * 1024u;
    fixture.Draw("quad_recording_recording.png");

    fixture.state.quadRecordingStatus = VideoRecordingIndicator{};
    fixture.state.quadRecording.layout = VideoMosaicLayout::Cross;
    fixture.state.quadRecording.cameras[1].width = 640;
    fixture.state.quadRecording.cameras[1].height = 360;
    Require(fixture.Draw("quad_recording_cross.png") == 4, "the cross shows the four cameras too");
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
        std::cout << "quad_recording_panel_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "quad_recording_panel_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
