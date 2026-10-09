#pragma once

#include <engine/renderer/camera.h>
#include <engine/renderer/render_types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace me
{

// Photo Mode (docs/design/2026-10-09-photo-mode-design.md): a still from the viewport's camera at a
// resolution of its own, rendered by a scene view beside the viewport's, so the viewport keeps its
// size and its frame rate while the photo is made.

// The sizes a photo may have, in pixels: the largest a fixed viewport resolution may have too.
inline constexpr uint32_t kPhotoMinSize = 64;
inline constexpr uint32_t kPhotoMaxSize = 8192;
// Frames the photo's view renders before it is saved, so TAA and the screen-space effects' histories
// have settled on the still camera. One frame is a picture without them.
inline constexpr uint32_t kPhotoMinWarmupFrames = 1;
inline constexpr uint32_t kPhotoMaxWarmupFrames = 256;

struct PhotoModeSettings
{
    bool operator==(const PhotoModeSettings&) const = default;

    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t warmupFrames = 32;
    // The viewport darkens what lies outside the photo while the Photo Mode window is open.
    bool framingGuide = true;
};

PhotoModeSettings ClampPhotoModeSettings(PhotoModeSettings settings);

// A photo as the backend last saw it, for the Photo Mode window: rendering (frames done of how many,
// at what size), or how the last one ended (where it was saved, or why it failed), shown for a few
// seconds from messageTime.
struct PhotoStatus
{
    bool rendering = false;
    uint32_t framesRendered = 0;
    uint32_t framesTotal = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string message;
    bool messageIsError = false;
    std::chrono::steady_clock::time_point messageTime{};
};

// What a photo's view needs of the GPU's memory, from what the viewport's costs per pixel, and what
// the GPU has free for it; unset before the render thread has measured. A view that does not fit
// fails to be made and the photo reports it.
struct PhotoMemoryEstimate
{
    uint64_t neededBytes = 0;
    uint64_t freeBytes = 0;
    bool Fits() const
    {
        return neededBytes <= freeBytes;
    }
};
std::optional<PhotoMemoryEstimate> EstimatePhotoMemory(const GpuMemoryReport& memory, uint32_t width, uint32_t height);

// The part of the viewport a photo shows, in the viewport's [0, 1] coordinates (x right, y down):
// the largest rectangle at the photo's aspect inside the viewport, centred. The photo keeps the
// viewport's horizontal or vertical field of view, whichever the rectangle spans.
struct PhotoFraming
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};
PhotoFraming ComputePhotoFraming(float viewportAspect, float photoAspect);

// The photo's camera: the viewport's, its vertical field of view narrowed to the framing's height,
// at the exposure the viewport shows now (auto exposure off), so the photo is what the framing
// guide outlines, as bright as it looks.
Camera PlacePhotoCamera(const Camera& viewport, float viewportAspect, float photoAspect);
}
