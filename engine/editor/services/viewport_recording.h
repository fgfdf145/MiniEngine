#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace me
{

// The viewport recording's settings (Tools > Record Viewport, the Recording window): the video's
// frame rate, its format and quality, and where it is saved. Its size is the viewport's, which the
// window sets through the viewport resolution (RenderDebugSettings::viewportResolution).

enum class ViewportRecordingFormat
{
    // H.264 through Media Foundation (Windows); falls back to Avi where that cannot encode.
    Mp4,
    // MJPEG: every frame a JPEG.
    Avi,
};

inline constexpr uint32_t kViewportRecordingMinFramesPerSecond = 1;
inline constexpr uint32_t kViewportRecordingMaxFramesPerSecond = 240;
inline constexpr uint32_t kViewportRecordingMaxMegabitsPerSecond = 400;
inline constexpr int kViewportRecordingMinJpegQuality = 1;
inline constexpr int kViewportRecordingMaxJpegQuality = 100;

struct ViewportRecordingSettings
{
    bool operator==(const ViewportRecordingSettings&) const = default;

    uint32_t framesPerSecond = 30;
    ViewportRecordingFormat format = ViewportRecordingFormat::Mp4;
    // The H.264 bit rate in Mbit/s; 0 for Mp4H264Writer::DefaultBitsPerSecond at the video's size.
    uint32_t megabitsPerSecond = 0;
    // stb's JPEG quality (Avi).
    int jpegQuality = 90;
    // Where recordings are saved; empty: the project's captures folder (ViewportRecordingFolder).
    std::string folder;
};

ViewportRecordingSettings ClampViewportRecordingSettings(ViewportRecordingSettings settings);

// The folder recordings go to: settings.folder, or ProjectRoot()/captures.
std::filesystem::path ViewportRecordingFolder(const ViewportRecordingSettings& settings);

// The file's extension, which picks the codec (VideoCodecForPath): ".mp4" when asked for and this
// build can encode it, else ".avi".
const char* ViewportRecordingExtension(const ViewportRecordingSettings& settings);
}
