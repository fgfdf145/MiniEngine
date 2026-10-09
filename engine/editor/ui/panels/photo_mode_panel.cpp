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
#include <filesystem>
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

constexpr std::array<std::pair<DlssMode, const char*>, 6> kPhotoDlssModes = {{
    {DlssMode::Off, "Off (TAA)"},
    {DlssMode::Dlaa, "DLAA"},
    {DlssMode::Quality, "Quality"},
    {DlssMode::Balanced, "Balanced"},
    {DlssMode::Performance, "Performance"},
    {DlssMode::UltraPerformance, "Ultra Performance"},
}};

// How the photo renders: the offline path tracer and its samples, and the photo's own DLSS.
void DrawPhotoRendering(PhotoModeSettings& settings, const EditorSharedState& state)
{
    ImGui::BeginDisabled(!state.pathTracingAvailable);
    ImGui::Checkbox("Offline Path Tracing", &settings.offlinePathTracing);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            state.pathTracingAvailable
                ? "Every tile is path traced in the offline mode until it has its samples, then held while the\n"
                  "warm-up frames let the resolve settle. Bounces, firefly clamp and light candidates are the\n"
                  "offline mode's (Graphics Debug > Path tracing). Off: rasterised with the ray traced effects."
                : "Needs a GPU with ray queries");
    }
    if (settings.offlinePathTracing && state.pathTracingAvailable)
    {
        ImGui::Indent();
        int samplesPerPixel = static_cast<int>(settings.samplesPerPixel);
        if (DragIntInRange("Samples per Frame", &samplesPerPixel, 1, static_cast<int>(kPhotoMaxSamplesPerPixel)))
        {
            settings.samplesPerPixel = static_cast<uint32_t>(samplesPerPixel);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Samples each pixel traces a frame. More finish sooner but keep the GPU busy longer each frame,\n"
                              "which the editor feels while the photo renders.");
        }
        int targetSamples = static_cast<int>(settings.targetSamples);
        if (DragIntInRange("Samples per Pixel", &targetSamples, static_cast<int>(kPhotoMinTargetSamples), static_cast<int>(kPhotoMaxTargetSamples)))
        {
            settings.targetSamples = static_cast<uint32_t>(targetSamples);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The samples each pixel of the photo accumulates in all, every tile alike");
        }
        ImGui::Unindent();
    }

    int selected = 0;
    for (size_t index = 0; index < kPhotoDlssModes.size(); ++index)
    {
        if (kPhotoDlssModes[index].first == settings.dlssMode)
        {
            selected = static_cast<int>(index);
        }
    }
    ImGui::BeginDisabled(!state.dlssAvailable);
    if (ImGui::BeginCombo("DLSS", kPhotoDlssModes[static_cast<size_t>(selected)].second))
    {
        for (size_t index = 0; index < kPhotoDlssModes.size(); ++index)
        {
            if (ImGui::Selectable(kPhotoDlssModes[index].second, selected == static_cast<int>(index)))
            {
                settings.dlssMode = kPhotoDlssModes[index].first;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            state.dlssAvailable ? "The photo's own DLSS, whatever the viewport runs: DLAA at the photo's size, the other modes\n"
                                  "rendered smaller and upscaled (less GPU memory, so larger tiles)"
                                : "DLSS is not available here: the engine's TAA resolves the photo");
    }
    ImGui::BeginDisabled(!state.dlssAvailable || !state.dlssRayReconstructionAvailable || settings.dlssMode == DlssMode::Off);
    ImGui::Checkbox("Ray Reconstruction", &settings.dlssRayReconstruction);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            state.dlssRayReconstructionAvailable ? "DLSS ray reconstruction denoises the path traced (or ray traced) light as it resolves"
                                                 : "The driver or the DLSS runtime has no ray reconstruction");
    }
}

// Where photos go: the folder, a button to choose another (or go back to captures/), and buttons
// that open the folder and the last photo.
void DrawPhotoFolder(PhotoModeSettings& settings, const PhotoStatus& status)
{
    const std::filesystem::path folder = PhotoFolder(settings);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(folder.string().c_str());
    ImGui::PopTextWrapPos();
    const bool choose = ImGui::Button(ICON_PH_FOLDER_OPEN " Choose Folder...");
    if (const std::optional<std::string> chosen = PickFilePath(FileDialogType::OpenFolder, choose))
    {
        settings.folder = *chosen;
    }
    if (!settings.folder.empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("Use captures/"))
        {
            settings.folder.clear();
        }
    }
    if (ImGui::Button(ICON_PH_FOLDER " Open Folder"))
    {
        // Made first, so a folder no photo has gone to yet still opens.
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        OpenInFileBrowser(folder);
    }
    std::error_code error;
    if (!status.lastPhoto.empty() && std::filesystem::exists(status.lastPhoto, error))
    {
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_IMAGE " Open Last Photo"))
        {
            OpenInFileBrowser(status.lastPhoto);
        }
    }
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
        const std::string samples = status.targetSamples > 0 ? fmt::format(", {} of {} spp", status.samples, status.targetSamples) : std::string();
        const std::string overlay =
            status.tileCount > 1
                ? fmt::format("{} x {}: tile {} of {}, frame {} of {}{}", status.width, status.height, status.tile, status.tileCount,
                              std::min(tileFrame, warmup), warmup, samples)
                : fmt::format("{} x {}: frame {} of {}{}", status.width, status.height, std::min(status.framesRendered, status.framesTotal),
                              status.framesTotal, samples);
        ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, 0.0f), overlay.c_str());
        if (!status.resolve.empty())
        {
            ImGui::TextDisabled("Resolved with %s", status.resolve.c_str());
        }
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
        const double extraBytes = PhotoExtraBytesPerPixel(
            settings.offlinePathTracing && state.pathTracingAvailable,
            state.dlssAvailable ? settings.dlssMode : DlssMode::Off,
            settings.dlssRayReconstruction && state.dlssRayReconstructionAvailable);
        const PhotoTiling tiling = PlanPhotoTiles(settings.width, settings.height, PhotoMaxViewPixels(state.gpuMemory, extraBytes));
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
        if (const std::optional<PhotoMemoryEstimate> memory = EstimatePhotoMemory(state.gpuMemory, view.width, view.height, extraBytes))
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

    ImGui::SeparatorText("Rendering");
    ImGui::BeginDisabled(busy);
    DrawPhotoRendering(settings, state);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Quality");
    int warmupFrames = static_cast<int>(settings.warmupFrames);
    if (DragIntInRange("Warm-up Frames", &warmupFrames, static_cast<int>(kPhotoMinWarmupFrames), static_cast<int>(kPhotoMaxWarmupFrames)))
    {
        settings.warmupFrames = static_cast<uint32_t>(warmupFrames);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Frames the photo's view renders before it is saved, so TAA or DLSS and the screen-space\n"
            "effects settle on the still camera; path traced, after its samples are in.");
    }
    ImGui::Checkbox("Framing Guide", &settings.framingGuide);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("While this window is open, the viewport darkens what lies outside the photo.");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "The photo is taken at the viewport's exposure from where the camera was when the shutter was pressed; "
        "the scene keeps moving, so hold driving still for a path traced one.");
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Save To");
    DrawPhotoFolder(settings, status);
    settings = ClampPhotoModeSettings(settings);
}
}
