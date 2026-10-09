#include "photo_mode_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/imgui_frame_snapshot.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui_colors.h>

#include <IconsPhosphor.h>
#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <utility>

namespace me
{

namespace
{
struct PhotoSizePreset
{
    const char* label;
    uint32_t width;
    uint32_t height;
};

constexpr std::array<PhotoSizePreset, 10> kPhotoSizePresets = {{
    {"1920 x 1080 (Full HD)", 1920, 1080},
    {"2560 x 1440 (QHD)", 2560, 1440},
    {"3840 x 2160 (4K UHD)", 3840, 2160},
    {"5120 x 2880 (5K)", 5120, 2880},
    {"7680 x 4320 (8K UHD)", 7680, 4320},
    {"4000 x 3000 (4:3)", 4000, 3000},
    {"6000 x 4000 (3:2)", 6000, 4000},
    {"2160 x 2160 (square)", 2160, 2160},
    {"2160 x 2700 (4:5 portrait)", 2160, 2700},
    {"2160 x 3840 (9:16 portrait)", 2160, 3840},
}};

// The photo's size: a preset, or Custom with its width and height, and a button that turns it
// between landscape and portrait.
void DrawPhotoSize(PhotoModeSettings& settings)
{
    int selected = static_cast<int>(kPhotoSizePresets.size());
    for (size_t index = 0; index < kPhotoSizePresets.size(); ++index)
    {
        if (kPhotoSizePresets[index].width == settings.width && kPhotoSizePresets[index].height == settings.height)
        {
            selected = static_cast<int>(index);
        }
    }
    const char* preview = selected < static_cast<int>(kPhotoSizePresets.size()) ? kPhotoSizePresets[static_cast<size_t>(selected)].label : "Custom";
    if (ImGui::BeginCombo("Resolution", preview))
    {
        for (size_t index = 0; index < kPhotoSizePresets.size(); ++index)
        {
            if (ImGui::Selectable(kPhotoSizePresets[index].label, selected == static_cast<int>(index)))
            {
                settings.width = kPhotoSizePresets[index].width;
                settings.height = kPhotoSizePresets[index].height;
            }
        }
        ImGui::EndCombo();
    }
    int size[2] = {static_cast<int>(settings.width), static_cast<int>(settings.height)};
    if (ImGui::InputInt2("Width x Height", size, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        settings.width = static_cast<uint32_t>(std::clamp(size[0], static_cast<int>(kPhotoMinSize), static_cast<int>(kPhotoMaxSize)));
        settings.height = static_cast<uint32_t>(std::clamp(size[1], static_cast<int>(kPhotoMinSize), static_cast<int>(kPhotoMaxSize)));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%u to %u pixels a side. Enter applies.", kPhotoMinSize, kPhotoMaxSize);
    }
    if (ImGui::Button(ICON_PH_ARROWS_CLOCKWISE " Swap Width and Height"))
    {
        std::swap(settings.width, settings.height);
    }
    const uint32_t divisor = std::gcd(settings.width, settings.height);
    ImGui::TextDisabled(
        "%.1f megapixels, %u:%u",
        static_cast<double>(settings.width) * static_cast<double>(settings.height) / 1.0e6,
        settings.width / std::max(divisor, 1u),
        settings.height / std::max(divisor, 1u));
}
}

PhotoModePanel::PhotoModePanel()
    : EditorPanel("photo_mode", "Photo Mode", ICON_PH_APERTURE)
{
}

void PhotoModePanel::OnGui(EditorContext& context)
{
    EditorSharedState& state = context.state;
    PhotoModeSettings& settings = state.photoMode;
    const PhotoStatus& status = state.photoStatus;

    const bool busy = status.rendering || status.saving;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_PH_CAMERA " Take Photo"))
    {
        context.result.actions.takePhoto = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Tools > Take Photo, Ctrl+F12");
    if (status.rendering)
    {
        const float fraction =
            status.framesTotal > 0 ? static_cast<float>(status.framesRendered) / static_cast<float>(status.framesTotal) : 0.0f;
        const uint32_t warmup = std::max(status.framesTotal / std::max(status.tileCount, 1u), 1u);
        const uint32_t tileFrame = status.framesRendered - (status.tile - 1) * warmup;
        const std::string overlay =
            status.tileCount > 1
                ? fmt::format("{} x {}: tile {} of {}, frame {} of {}", status.width, status.height, status.tile, status.tileCount, tileFrame, warmup)
                : fmt::format("{} x {}: frame {} of {}", status.width, status.height, status.framesRendered, status.framesTotal);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
        // The view as it draws the photo (a tile with its guard, when it is tiled), across the window at
        // its aspect; the render thread puts the picture in, or leaves it out until the view has one.
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        const float height =
            width * static_cast<float>(std::max(status.viewHeight, 1u)) / static_cast<float>(std::max(status.viewWidth, 1u));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(0, 0, 0, 255));
        drawList->AddImage(kPhotoViewTextureId, origin, ImVec2(origin.x + width, origin.y + height));
        ImGui::Dummy(ImVec2(width, height));
    }
    if (status.saving)
    {
        const std::string overlay = fmt::format("Writing the {} x {} PNG", status.width, status.height);
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f), overlay.c_str());
    }
    constexpr double kMessageSeconds = 8.0;
    if (!busy && !status.message.empty() &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - status.messageTime).count() < kMessageSeconds)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(status.messageIsError ? ui_colors::kTextDanger : ui_colors::kTextAccent, "%s", status.message.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::SeparatorText("Resolution");
    // A photo keeps the size it started with.
    ImGui::BeginDisabled(busy);
    DrawPhotoSize(settings);
    ImGui::EndDisabled();
    // How it would be rendered now: in one view, or in tiles small enough for the free memory. While
    // a photo renders its own view is in the usage already, so the plan is left as it was.
    if (!busy)
    {
        const PhotoTiling tiling = PlanPhotoTiles(settings.width, settings.height, PhotoMaxViewPixels(state.gpuMemory));
        const RenderExtent view = tiling.ViewExtent();
        if (tiling.Tiled())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(
                "Renders in %zu tiles (%u x %u) of %u x %u, each with a %u-pixel border that is cropped",
                tiling.tiles.size(),
                tiling.columns,
                tiling.rows,
                tiling.tileWidth,
                tiling.tileHeight,
                tiling.guard);
            ImGui::PopTextWrapPos();
        }
        if (const std::optional<PhotoMemoryEstimate> memory = EstimatePhotoMemory(state.gpuMemory, view.width, view.height))
        {
            constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
            const std::string line = fmt::format(
                "{} about {:.1f} GB of GPU memory, {:.1f} GB free",
                tiling.Tiled() ? "Each tile needs" : "Needs",
                static_cast<double>(memory->neededBytes) / kGigabyte,
                static_cast<double>(memory->freeBytes) / kGigabyte);
            if (memory->Fits())
            {
                ImGui::TextDisabled("%s", line.c_str());
            }
            else
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ui_colors::kTextWarning, "%s: the GPU is too full even for the smallest tiles.", line.c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The viewport keeps its own resolution; the photo renders beside it.");
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Quality");
    int warmupFrames = static_cast<int>(settings.warmupFrames);
    if (DragIntInRange("Warm-up Frames", &warmupFrames, static_cast<int>(kPhotoMinWarmupFrames), static_cast<int>(kPhotoMaxWarmupFrames)))
    {
        settings.warmupFrames = static_cast<uint32_t>(warmupFrames);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Frames the photo's view renders before it is saved, so TAA and the screen-space\n"
            "effects settle on the still camera. Hold the camera still while they render.");
    }
    ImGui::Checkbox("Framing Guide", &settings.framingGuide);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("While this window is open, the viewport darkens what lies outside the photo.");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "The photo is resolved with TAA at the viewport's exposure from where the camera was when the shutter "
        "was pressed, and saved under captures/.");
    ImGui::PopTextWrapPos();
    settings = ClampPhotoModeSettings(settings);
}
}
