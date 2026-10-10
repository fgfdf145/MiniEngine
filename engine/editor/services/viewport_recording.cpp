#include "viewport_recording.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/core/video/mp4_h264_writer.h>

#include <algorithm>

namespace me
{

ViewportRecordingSettings ClampViewportRecordingSettings(ViewportRecordingSettings settings)
{
    settings.framesPerSecond =
        std::clamp(settings.framesPerSecond, kViewportRecordingMinFramesPerSecond, kViewportRecordingMaxFramesPerSecond);
    settings.megabitsPerSecond = std::min(settings.megabitsPerSecond, kViewportRecordingMaxMegabitsPerSecond);
    settings.jpegQuality = std::clamp(settings.jpegQuality, kViewportRecordingMinJpegQuality, kViewportRecordingMaxJpegQuality);
    return settings;
}

std::filesystem::path ViewportRecordingFolder(const ViewportRecordingSettings& settings)
{
    if (!settings.folder.empty())
    {
        return std::filesystem::path(settings.folder);
    }
    return EnginePaths::ProjectRoot() / "captures";
}

const char* ViewportRecordingExtension(const ViewportRecordingSettings& settings)
{
    return settings.format == ViewportRecordingFormat::Mp4 && Mp4H264Writer::IsSupported() ? ".mp4" : ".avi";
}
}
