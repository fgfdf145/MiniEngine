#include "recording_panel.h"

#include <engine/core/video/mp4_h264_writer.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui_colors.h>

#include <IconsPhosphor.h>
#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>

namespace me
{

namespace
{
constexpr std::array<uint32_t, 6> kFrameRatePresets = {24, 25, 30, 50, 60, 120};

// The size the next recording takes: the fixed viewport resolution, or the panel's.
void DrawRecordingSize(EditorSharedState& state, bool recording)
{
    ImGui::BeginDisabled(recording);
    DrawViewportResolution(state.renderDebug.viewportResolution, state.forcedViewportExtent);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    if (!state.renderDebug.viewportResolution.fixed && !state.forcedViewportExtent.has_value())
    {
        ImGui::TextDisabled("The video takes the viewport panel's size when it starts, and keeps it.");
    }
    ImGui::TextDisabled("The same setting as Graphics Debug's. The render scale and DLSS apply to it.");
    ImGui::PopTextWrapPos();
}

void DrawFrameRate(ViewportRecordingSettings& settings)
{
    const std::string preview = fmt::format("{} fps", settings.framesPerSecond);
    if (ImGui::BeginCombo("Frame Rate", preview.c_str()))
    {
        for (const uint32_t rate : kFrameRatePresets)
        {
            const std::string label = fmt::format("{} fps", rate);
            if (ImGui::Selectable(label.c_str(), settings.framesPerSecond == rate))
            {
                settings.framesPerSecond = rate;
            }
        }
        ImGui::EndCombo();
    }
    int framesPerSecond = static_cast<int>(settings.framesPerSecond);
    if (DragIntInRange(
            "Custom Rate",
            &framesPerSecond,
            static_cast<int>(kViewportRecordingMinFramesPerSecond),
            static_cast<int>(kViewportRecordingMaxFramesPerSecond)))
    {
        settings.framesPerSecond = static_cast<uint32_t>(framesPerSecond);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The video plays at the speed things happened: a frame the editor did not draw in time is\n"
            "repeated, so above the editor's own frame rate the video only grows, not smoother.");
    }
}

void DrawFormat(ViewportRecordingSettings& settings, RenderExtent size)
{
    const bool mp4Supported = Mp4H264Writer::IsSupported();
    int format = settings.format == ViewportRecordingFormat::Avi ? 1 : 0;
    ImGui::BeginDisabled(!mp4Supported);
    ImGui::RadioButton("MP4 (H.264)", &format, 0);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            mp4Supported ? "Small, and sites take it as is. Encoded by the GPU where Media Foundation finds its encoder."
                         : "Needs Windows Media Foundation: recordings are AVI here");
    }
    ImGui::SameLine();
    ImGui::RadioButton("AVI (MJPEG)", &format, 1);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Every frame a JPEG: large files, any platform. At 3.75 GB it continues in _2.avi, _3 ...");
    }
    settings.format = format == 1 ? ViewportRecordingFormat::Avi : ViewportRecordingFormat::Mp4;

    if (settings.format == ViewportRecordingFormat::Mp4 && mp4Supported)
    {
        bool automatic = settings.megabitsPerSecond == 0;
        const uint32_t automaticBits = size.IsValid() ? Mp4H264Writer::DefaultBitsPerSecond(size.width, size.height, settings.framesPerSecond) : 0;
        if (ImGui::Checkbox("Automatic Bit Rate", &automatic))
        {
            settings.megabitsPerSecond = automatic ? 0 : std::max(automaticBits / 1'000'000u, 1u);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("0.2 bits a pixel a frame, from 4 to 120 Mbit/s: keeps detail through a site's own re-encode");
        }
        if (automatic)
        {
            if (automaticBits > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.0f Mbit/s at %u x %u", static_cast<double>(automaticBits) / 1.0e6, size.width, size.height);
            }
        }
        else
        {
            int megabits = static_cast<int>(settings.megabitsPerSecond);
            if (DragIntInRange("Bit Rate (Mbit/s)", &megabits, 1, static_cast<int>(kViewportRecordingMaxMegabitsPerSecond)))
            {
                settings.megabitsPerSecond = static_cast<uint32_t>(megabits);
            }
        }
    }
    else
    {
        DragIntInRange("JPEG Quality", &settings.jpegQuality, kViewportRecordingMinJpegQuality, kViewportRecordingMaxJpegQuality);
    }
}

void DrawFolder(ViewportRecordingSettings& settings, const VideoRecordingIndicator& status)
{
    const std::filesystem::path folder = ViewportRecordingFolder(settings);
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
        // Made first, so a folder no recording has gone to yet still opens.
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        OpenInFileBrowser(folder);
    }
    std::error_code error;
    if (!status.lastFile.empty() && std::filesystem::exists(status.lastFile, error))
    {
        ImGui::SameLine();
        if (ImGui::Button(ICON_PH_FILE_VIDEO " Open Last Recording"))
        {
            OpenInFileBrowser(status.lastFile);
        }
    }
}
}

RecordingPanel::RecordingPanel()
    : EditorPanel("recording", "Recording", ICON_PH_VIDEO_CAMERA)
{
}

void RecordingPanel::OnGui(EditorContext& context)
{
    EditorSharedState& state = context.state;
    ViewportRecordingSettings& settings = state.viewportRecording;
    const VideoRecordingIndicator& status = state.videoRecording;
    const bool recording = status.active;

    if (recording)
    {
        if (ImGui::Button(ICON_PH_STOP " Stop"))
        {
            context.result.actions.toggleVideoRecording = true;
        }
        ImGui::SameLine();
        const int totalSeconds = static_cast<int>(status.seconds);
        std::string line = fmt::format(
            "REC  {:02}:{:02}  {:.1f} MB", totalSeconds / 60, totalSeconds % 60, static_cast<double>(status.bytes) / (1024.0 * 1024.0));
        if (status.droppedFrames > 0)
        {
            line += fmt::format("  ({} frames dropped)", status.droppedFrames);
        }
        ImGui::TextColored(ui_colors::kTextDanger, "%s", line.c_str());
    }
    else
    {
        if (ImGui::Button(ICON_PH_RECORD " Record"))
        {
            context.result.actions.toggleVideoRecording = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Tools > Record Viewport, Shift+F12");
        constexpr double kMessageSeconds = 8.0;
        if (!status.message.empty() &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - status.messageTime).count() < kMessageSeconds)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(status.messageIsError ? ui_colors::kTextDanger : ui_colors::kTextAccent, "%s", status.message.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // A recording keeps the settings it started with.
    ImGui::SeparatorText("Size");
    DrawRecordingSize(state, recording);

    ImGui::BeginDisabled(recording);
    ImGui::SeparatorText("Video");
    DrawFrameRate(settings);
    const ViewportResolutionSettings& resolution = state.renderDebug.viewportResolution;
    const RenderExtent size = state.forcedViewportExtent.value_or(
        resolution.fixed ? RenderExtent{static_cast<uint32_t>(resolution.width), static_cast<uint32_t>(resolution.height)} : RenderExtent{});
    DrawFormat(settings, size);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The picture is the viewport's, as Graphics Debug renders it (DLSS, ray reconstruction, path tracing).");
    ImGui::PopTextWrapPos();

    // Read when a recording starts, so it may change while one runs.
    ImGui::SeparatorText("Save To");
    DrawFolder(settings, status);
    settings = ClampViewportRecordingSettings(settings);
}
}
