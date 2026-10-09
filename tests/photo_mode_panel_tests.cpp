// The Photo Mode panel drawn headless: idle with room for the photo, an 8K photo planned in tiles,
// while a tiled photo renders (its preview stood in for by a test card), while it is written, and
// after one was saved.
// Drawing each checks the panel's disabled scopes stay balanced and that its button asks for a
// photo; with MINIENGINE_UI_SNAPSHOT_DIR set each is written there as a PNG to look at.

#include <engine/editor/command_registry.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/imgui_frame_snapshot.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_style.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/photo_mode_panel.h>
#include <engine/logic/editor_scene.h>

#include <imgui.h>

#include "imgui_software_raster.h"

#include <chrono>
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
constexpr int kWidth = 480;
constexpr int kHeight = 640;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

// A picture for the preview: a gradient, as the render thread would put the photo's view there.
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

// What the render thread does to the preview: here, the test card in its place.
size_t ServePhotoPicture(ImDrawData& drawData)
{
    size_t served = 0;
    for (ImDrawList* list : drawData.CmdLists)
    {
        for (ImDrawCmd& command : list->CmdBuffer)
        {
            if (command.TexRef._TexID == kPhotoViewTextureId)
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
    PhotoModePanel panel;

    Fixture()
    {
        // An 8 GB GPU with 3 GB free, 300 bytes a pixel for a view.
        state.gpuMemory.serial = 10;
        state.gpuMemory.budget = uint64_t{8} << 30;
        state.gpuMemory.usage = uint64_t{5} << 30;
        state.gpuMemory.viewBytesPerPixel = 300.0;
        panel.Open();
    }

    // Two frames, so the window has its size; the second is the one looked at. Returns how many
    // times it drew the photo's picture.
    size_t Draw(const char* snapshot)
    {
        size_t pictures = 0;
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
            pictures = ServePhotoPicture(*ImGui::GetDrawData());
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
    // A GPU with ray queries, DLSS and ray reconstruction, as the backend reports them.
    fixture.state.pathTracingAvailable = true;
    fixture.state.dlssAvailable = true;
    fixture.state.dlssRayReconstructionAvailable = true;
    Require(fixture.Draw("photo_mode_idle.png") == 0, "no preview while nothing renders");

    fixture.state.photoMode.width = 7680;
    fixture.state.photoMode.height = 4320;
    fixture.Draw("photo_mode_8k_tiles.png");

    fixture.state.photoStatus.rendering = true;
    fixture.state.photoStatus.tile = 3;
    fixture.state.photoStatus.tileCount = 12;
    fixture.state.photoStatus.framesRendered = 2 * 32 + 10;
    fixture.state.photoStatus.framesTotal = 12 * 32;
    fixture.state.photoStatus.width = 7680;
    fixture.state.photoStatus.height = 4320;
    fixture.state.photoStatus.viewWidth = 2176;
    fixture.state.photoStatus.viewHeight = 1696;
    fixture.state.photoStatus.samples = 512;
    fixture.state.photoStatus.targetSamples = 1024;
    fixture.state.photoStatus.resolve = "DLSS ray reconstruction";
    Require(fixture.Draw("photo_mode_rendering.png") == 1, "the preview shows the photo's view while it renders");

    fixture.state.photoStatus.rendering = false;
    fixture.state.photoStatus.saving = true;
    Require(fixture.Draw("photo_mode_saving.png") == 0, "no preview while the PNG is written");

    fixture.state.photoStatus = PhotoStatus{};
    fixture.state.photoStatus.message = "Saved 3840 x 2160 to C:/Project/MiniEngine/captures/photo_20261009_161000.png";
    fixture.state.photoStatus.messageTime = std::chrono::steady_clock::now();
    fixture.state.photoStatus.lastPhoto = std::filesystem::temp_directory_path();
    fixture.Draw("photo_mode_saved.png");

    // Rasterised with TAA, saved to a folder of its own; and on a GPU without ray queries or DLSS.
    fixture.state.photoMode.offlinePathTracing = false;
    fixture.state.photoMode.dlssMode = DlssMode::Off;
    fixture.state.photoMode.folder = "D:/Photos";
    fixture.Draw("photo_mode_raster.png");
    fixture.state.pathTracingAvailable = false;
    fixture.state.dlssAvailable = false;
    fixture.Draw("photo_mode_no_rays.png");
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
        std::cout << "photo_mode_panel_tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "photo_mode_panel_tests failed: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
