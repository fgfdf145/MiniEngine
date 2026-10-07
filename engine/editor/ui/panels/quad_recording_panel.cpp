#include "quad_recording_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/imgui_frame_snapshot.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui_colors.h>

#include <IconsPhosphor.h>
#include <engine/core/log/log.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace me
{

namespace
{
struct ResolutionPreset
{
    const char* label;
    uint32_t width;
    uint32_t height;
};

constexpr std::array<ResolutionPreset, 5> kResolutionPresets = {{
    {"640 x 360", 640, 360},
    {"960 x 540", 960, 540},
    {"1280 x 720", 1280, 720},
    {"1600 x 900", 1600, 900},
    {"1920 x 1080", 1920, 1080},
}};

// A camera's picture size: a preset, or Custom with its width and height.
void DrawResolution(QuadCameraSettings& camera)
{
    int selected = static_cast<int>(kResolutionPresets.size());
    for (size_t index = 0; index < kResolutionPresets.size(); ++index)
    {
        if (kResolutionPresets[index].width == camera.width && kResolutionPresets[index].height == camera.height)
        {
            selected = static_cast<int>(index);
        }
    }
    const char* preview = selected < static_cast<int>(kResolutionPresets.size()) ? kResolutionPresets[static_cast<size_t>(selected)].label : "Custom";
    if (ImGui::BeginCombo("Resolution", preview))
    {
        for (size_t index = 0; index < kResolutionPresets.size(); ++index)
        {
            if (ImGui::Selectable(kResolutionPresets[index].label, selected == static_cast<int>(index)))
            {
                camera.width = kResolutionPresets[index].width;
                camera.height = kResolutionPresets[index].height;
            }
        }
        ImGui::EndCombo();
    }
    int width = static_cast<int>(camera.width);
    int height = static_cast<int>(camera.height);
    if (DragIntInRange("Width", &width, static_cast<int>(kQuadCameraMinSize), static_cast<int>(kQuadCameraMaxSize)))
    {
        camera.width = static_cast<uint32_t>(width);
    }
    if (DragIntInRange("Height", &height, static_cast<int>(kQuadCameraMinSize), static_cast<int>(kQuadCameraMaxSize)))
    {
        camera.height = static_cast<uint32_t>(height);
    }
}

// The four cameras' pictures laid out as the video will have them, as wide as the window.
void DrawPreview(const QuadRecordingSettings& settings, bool live)
{
    const VideoMosaic mosaic = ComputeQuadMosaic(settings);
    const float width = ImGui::GetContentRegionAvail().x;
    if (mosaic.width == 0 || mosaic.height == 0 || width <= 1.0f)
    {
        return;
    }
    const float scale = width / static_cast<float>(mosaic.width);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, std::floor(static_cast<float>(mosaic.height) * scale));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 255));
    for (size_t index = 0; index < mosaic.tiles.size() && index < kCaptureViewTextureIds.size(); ++index)
    {
        const VideoMosaicTile& tile = mosaic.tiles[index];
        const ImVec2 min(origin.x + static_cast<float>(tile.x) * scale, origin.y + static_cast<float>(tile.y) * scale);
        const ImVec2 max(min.x + static_cast<float>(tile.width) * scale, min.y + static_cast<float>(tile.height) * scale);
        if (live)
        {
            // The render thread puts the camera's picture of this frame in its place, or leaves the
            // tile out while the camera has none.
            drawList->AddImage(kCaptureViewTextureIds[index], min, max);
        }
        drawList->AddRect(min, max, IM_COL32(255, 255, 255, 60));
        drawList->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), IM_COL32(255, 255, 255, 220), QuadCameraSlotName(static_cast<QuadCameraSlot>(index)));
    }
    ImGui::Dummy(size);
}

void DrawCamera(QuadCameraSettings& camera, const QuadCameraSettings& defaults, bool recording)
{
    // A video has one size: the pictures keep theirs while it records.
    ImGui::BeginDisabled(recording);
    DrawResolution(camera);
    ImGui::EndDisabled();
    DragFloat3InRange("Position (R, U, F)", &camera.position.x, -100.0f, 100.0f, "%.2f m", 0.02f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("From the car's origin, in metres: to its right, up, forward.");
    }
    DragFloat3InRange("Look At (R, U, F)", &camera.target.x, -100.0f, 100.0f, "%.2f m", 0.02f);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The point the camera looks at, from the car's origin: to its right, up, forward.");
    }
    DragFloatInRange("Field of View", &camera.fovDegrees, 5.0f, 120.0f, "%.1f deg");
    ImGui::Checkbox("Tilt With Body", &camera.followBodyTilt);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "On: fixed to the body, it pitches and rolls with it.\n"
            "Off: it turns with the car's heading alone and keeps the horizon level,\n"
            "so the body's roll and pitch show in the picture.");
    }
    if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE " Reset Camera"))
    {
        const uint32_t width = camera.width;
        const uint32_t height = camera.height;
        camera = defaults;
        if (recording)
        {
            camera.width = width;
            camera.height = height;
        }
    }
}
}

QuadRecordingPanel::QuadRecordingPanel()
    : EditorPanel("quad_recording", "Quad Recording", ICON_PH_SQUARES_FOUR)
{
}

void QuadRecordingPanel::OnGui(EditorContext& context)
{
    EditorSharedState& state = context.state;
    QuadRecordingSettings& settings = state.quadRecording;
    const VideoRecordingIndicator& status = state.quadRecordingStatus;
    const bool recording = status.active;
    const bool hasTarget = !state.quadRecordingTarget.empty();

    if (recording)
    {
        if (ImGui::Button(ICON_PH_STOP " Stop"))
        {
            context.result.actions.toggleQuadRecording = true;
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
        ImGui::BeginDisabled(!hasTarget);
        if (ImGui::Button(ICON_PH_RECORD " Record"))
        {
            context.result.actions.toggleQuadRecording = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Tools > Record Quad Cameras, Ctrl+Shift+F12");
    }
    if (hasTarget)
    {
        ImGui::TextDisabled("Following %s", state.quadRecordingTarget.c_str());
    }
    else
    {
        ImGui::TextColored(ui_colors::kTextWarning, "Nothing to film: drive a car (Play) or select one.");
    }
    constexpr double kMessageSeconds = 8.0;
    if (!recording && !status.message.empty() &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - status.messageTime).count() < kMessageSeconds)
    {
        ImGui::TextColored(status.messageIsError ? ui_colors::kTextDanger : ui_colors::kTextAccent, "%s", status.message.c_str());
    }

    ImGui::SeparatorText("Video");
    ImGui::BeginDisabled(recording);
    int layout = static_cast<int>(settings.layout);
    static constexpr const char* kLayouts[] = {"2 x 2 grid (front, rear / left, right)", "Cross (each side where it looks from)"};
    if (ImGui::Combo("Layout", &layout, kLayouts, IM_ARRAYSIZE(kLayouts)))
    {
        settings.layout = static_cast<VideoMosaicLayout>(layout);
    }
    int framesPerSecond = static_cast<int>(settings.framesPerSecond);
    if (DragIntInRange("Frames per Second", &framesPerSecond, 1, 120))
    {
        settings.framesPerSecond = static_cast<uint32_t>(framesPerSecond);
    }
    ImGui::Checkbox("Camera Names", &settings.labels);
    ImGui::EndDisabled();
    const VideoMosaic mosaic = ComputeQuadMosaic(ClampQuadRecordingSettings(settings));
    ImGui::TextDisabled("Canvas %u x %u, saved under captures/", mosaic.width, mosaic.height);

    ImGui::Checkbox("Preview", &m_preview);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Each camera renders the whole scene: the preview costs as much as recording.");
    }
    // Asked for again each frame the window draws, so the cameras stop rendering once it closes.
    state.quadRecordingPreview = m_preview;
    if (m_preview || recording)
    {
        DrawPreview(ClampQuadRecordingSettings(settings), hasTarget);
    }

    ImGui::SeparatorText("Cameras");
    const std::array<QuadCameraSettings, kQuadCameraCount> defaults = DefaultQuadCameras();
    for (size_t index = 0; index < kQuadCameraCount; ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::CollapsingHeader(QuadCameraSlotName(static_cast<QuadCameraSlot>(index)), index == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            DrawCamera(settings.cameras[index], defaults[index], recording);
        }
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_PH_ARROW_COUNTER_CLOCKWISE " Reset All Cameras"))
    {
        for (size_t index = 0; index < kQuadCameraCount; ++index)
        {
            const uint32_t width = settings.cameras[index].width;
            const uint32_t height = settings.cameras[index].height;
            settings.cameras[index] = defaults[index];
            if (recording)
            {
                settings.cameras[index].width = width;
                settings.cameras[index].height = height;
            }
        }
    }
    settings = ClampQuadRecordingSettings(settings);
}
}
