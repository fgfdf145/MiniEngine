#pragma once

#include <engine/renderer/camera.h>
#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace me
{

// Photo Mode (docs/design/2026-10-09-photo-mode-design.md): a still from the viewport's camera at a
// resolution of its own, rendered by a scene view beside the viewport's, so the viewport keeps its
// size and its frame rate while the photo is made. A photo larger than one view can be renders in
// tiles (docs/design/2026-10-09-photo-tiles-design.md).

// The sizes a photo may have, in pixels: the largest a fixed viewport resolution may have too.
inline constexpr uint32_t kPhotoMinSize = 64;
inline constexpr uint32_t kPhotoMaxSize = 8192;
// Frames the photo's view renders before it is saved, so TAA and the screen-space effects' histories
// have settled on the still camera. One frame is a picture without them. Each tile renders as many.
inline constexpr uint32_t kPhotoMinWarmupFrames = 1;
inline constexpr uint32_t kPhotoMaxWarmupFrames = 256;

// The most pixels one view of a photo may have before it is cut into tiles: 4K UHD, which takes some
// 3.5 GB on the GPU (the photo view's own memory, GpuMemoryReport::viewBytesPerPixel). Less when the
// GPU has less free.
inline constexpr uint64_t kPhotoMaxViewPixels = uint64_t{3840} * 2160;
// A tile's side at most, and at least: fewer, larger tiles are faster; a GPU too full for the
// smallest gets no photo.
inline constexpr uint32_t kPhotoMaxTileSide = 2048;
inline constexpr uint32_t kPhotoMinTileSide = 512;
// Pixels each tile renders past its edges on every side and drops, so the screen-space effects near
// its edges (ambient occlusion, indirect light, reflections, the glare's nearer rings) see what lies
// beyond them, as they would in one view of the whole photo, and the tiles meet without seams.
inline constexpr uint32_t kPhotoTileGuard = 128;

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
// the tile among how many, at what size) or saving, or how the last one ended (where it was saved,
// or why it failed), shown for a few seconds from messageTime.
struct PhotoStatus
{
    bool rendering = false;
    bool saving = false;
    uint32_t framesRendered = 0;
    uint32_t framesTotal = 0;
    uint32_t tile = 0;
    uint32_t tileCount = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    // The size of the view rendering it: the photo's, or one tile's with its guard.
    uint32_t viewWidth = 0;
    uint32_t viewHeight = 0;
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

// How many pixels one view of a photo may have: kPhotoMaxViewPixels, or what fits in most of the
// GPU's free memory when the render thread has measured it.
uint64_t PhotoMaxViewPixels(const GpuMemoryReport& memory);

// One tile: the part of the photo it fills, in the photo's pixels (x right, y down from the top-left
// corner), clipped to the photo.
struct PhotoTile
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// How a photo is rendered: in one view of its own size (one tile, no guard), or in a grid of equal
// tiles, each rendered with kPhotoTileGuard more pixels on every side. Every tile's view has the same
// size (ViewExtent), so one view renders them all in turn; tiles in the last column or row reach
// past the photo's edge, and what lies past it is dropped with the guard.
struct PhotoTiling
{
    uint32_t photoWidth = 0;
    uint32_t photoHeight = 0;
    uint32_t columns = 1;
    uint32_t rows = 1;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    uint32_t guard = 0;
    std::vector<PhotoTile> tiles;

    bool Tiled() const
    {
        return tiles.size() > 1;
    }
    RenderExtent ViewExtent() const
    {
        return RenderExtent{tileWidth + 2 * guard, tileHeight + 2 * guard};
    }
};

// One view when the photo has at most maxViewPixels; else the fewest tiles of a side no longer than
// the largest square view within maxViewPixels with its guard (kPhotoMinTileSide to
// kPhotoMaxTileSide), the photo divided evenly between them, columns first along the top row.
PhotoTiling PlanPhotoTiles(uint32_t width, uint32_t height, uint64_t maxViewPixels);

// The whole photo's projection narrowed to one tile's view: the tile's rectangle and its guard
// become the view's [-1, 1], an off-centre frustum through the same camera. For the projection the
// GPU renders with on a backend whose Y axis is inverted (Camera::GetProjectionMatrix), invertedY.
glm::mat4 TileProjection(const glm::mat4& projection, const PhotoTiling& tiling, const PhotoTile& tile, bool invertedY);

// Copies the tile's part of its view's picture (RGBA8, tiling.ViewExtent() in size, rows from the
// top) into the photo's canvas (RGBA8, the photo's size), dropping the guard and anything past the
// photo's edge.
void CopyTileIntoCanvas(
    const PhotoTiling& tiling,
    const PhotoTile& tile,
    const std::vector<uint8_t>& viewPixels,
    std::vector<uint8_t>& canvas);

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
