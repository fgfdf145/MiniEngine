#include "photo_mode.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace me
{

PhotoModeSettings ClampPhotoModeSettings(PhotoModeSettings settings)
{
    settings.width = std::clamp(settings.width, kPhotoMinSize, kPhotoMaxSize);
    settings.height = std::clamp(settings.height, kPhotoMinSize, kPhotoMaxSize);
    settings.warmupFrames = std::clamp(settings.warmupFrames, kPhotoMinWarmupFrames, kPhotoMaxWarmupFrames);
    return settings;
}

std::optional<PhotoMemoryEstimate> EstimatePhotoMemory(const GpuMemoryReport& memory, uint32_t width, uint32_t height)
{
    if (memory.serial == 0 || memory.budget == 0 || !(memory.viewBytesPerPixel > 0.0))
    {
        return std::nullopt;
    }
    PhotoMemoryEstimate estimate;
    estimate.neededBytes = static_cast<uint64_t>(memory.viewBytesPerPixel * static_cast<double>(width) * static_cast<double>(height));
    estimate.freeBytes = memory.budget > memory.usage ? memory.budget - memory.usage : 0;
    return estimate;
}

uint64_t PhotoMaxViewPixels(const GpuMemoryReport& memory)
{
    // Most of what is free: the frames in flight and the streaming keep moving what the rest holds.
    constexpr double kUsableShare = 0.75;
    if (memory.serial == 0 || memory.budget == 0 || !(memory.viewBytesPerPixel > 0.0))
    {
        return kPhotoMaxViewPixels;
    }
    const uint64_t freeBytes = memory.budget > memory.usage ? memory.budget - memory.usage : 0;
    const auto fitting = static_cast<uint64_t>(static_cast<double>(freeBytes) * kUsableShare / memory.viewBytesPerPixel);
    return std::min(fitting, kPhotoMaxViewPixels);
}

PhotoTiling PlanPhotoTiles(uint32_t width, uint32_t height, uint64_t maxViewPixels)
{
    PhotoTiling tiling;
    tiling.photoWidth = std::max(width, 1u);
    tiling.photoHeight = std::max(height, 1u);
    if (static_cast<uint64_t>(tiling.photoWidth) * tiling.photoHeight <= maxViewPixels)
    {
        tiling.tileWidth = tiling.photoWidth;
        tiling.tileHeight = tiling.photoHeight;
        tiling.tiles.push_back(PhotoTile{0, 0, tiling.photoWidth, tiling.photoHeight});
        return tiling;
    }
    // The largest square view that fits, less its guard on both sides.
    const auto viewSide = static_cast<uint32_t>(std::sqrt(static_cast<double>(maxViewPixels)));
    const uint32_t side =
        std::clamp(viewSide > 2 * kPhotoTileGuard ? viewSide - 2 * kPhotoTileGuard : 0u, kPhotoMinTileSide, kPhotoMaxTileSide);
    tiling.guard = kPhotoTileGuard;
    tiling.columns = (tiling.photoWidth + side - 1) / side;
    tiling.rows = (tiling.photoHeight + side - 1) / side;
    tiling.tileWidth = (tiling.photoWidth + tiling.columns - 1) / tiling.columns;
    tiling.tileHeight = (tiling.photoHeight + tiling.rows - 1) / tiling.rows;
    for (uint32_t row = 0; row < tiling.rows; ++row)
    {
        for (uint32_t column = 0; column < tiling.columns; ++column)
        {
            PhotoTile tile;
            tile.x = column * tiling.tileWidth;
            tile.y = row * tiling.tileHeight;
            if (tile.x >= tiling.photoWidth || tile.y >= tiling.photoHeight)
            {
                continue;
            }
            tile.width = std::min(tiling.tileWidth, tiling.photoWidth - tile.x);
            tile.height = std::min(tiling.tileHeight, tiling.photoHeight - tile.y);
            tiling.tiles.push_back(tile);
        }
    }
    return tiling;
}

glm::mat4 TileProjection(const glm::mat4& projection, const PhotoTiling& tiling, const PhotoTile& tile, bool invertedY)
{
    const float width = static_cast<float>(tiling.photoWidth);
    const float height = static_cast<float>(tiling.photoHeight);
    const float guard = static_cast<float>(tiling.guard);
    // The view's rectangle in the photo's pixels: the tile at its full size, with its guard.
    const float left = static_cast<float>(tile.x) - guard;
    const float right = static_cast<float>(tile.x + tiling.tileWidth) + guard;
    const float top = static_cast<float>(tile.y) - guard;
    const float bottom = static_cast<float>(tile.y + tiling.tileHeight) + guard;
    // The same in the whole photo's NDC, Y up (row 0 at +1).
    const float ndcLeft = 2.0f * left / width - 1.0f;
    const float ndcRight = 2.0f * right / width - 1.0f;
    const float ndcTop = 1.0f - 2.0f * top / height;
    const float ndcBottom = 1.0f - 2.0f * bottom / height;
    const float centreX = 0.5f * (ndcLeft + ndcRight);
    const float scaleX = 0.5f * (ndcRight - ndcLeft);
    // An inverted Y axis negates the projection's Y row, so the centre's sign turns with it.
    const float centreY = (invertedY ? -0.5f : 0.5f) * (ndcTop + ndcBottom);
    const float scaleY = 0.5f * (ndcTop - ndcBottom);
    // clip.x' = (clip.x - centre * clip.w) / scale, and Y the same: row by row (glm is column-major).
    glm::mat4 narrowed = projection;
    for (int column = 0; column < 4; ++column)
    {
        narrowed[column][0] = (projection[column][0] - centreX * projection[column][3]) / scaleX;
        narrowed[column][1] = (projection[column][1] - centreY * projection[column][3]) / scaleY;
    }
    return narrowed;
}

void CopyTileIntoCanvas(
    const PhotoTiling& tiling,
    const PhotoTile& tile,
    const std::vector<uint8_t>& viewPixels,
    std::vector<uint8_t>& canvas)
{
    const RenderExtent view = tiling.ViewExtent();
    if (viewPixels.size() < static_cast<size_t>(view.width) * view.height * 4 ||
        canvas.size() < static_cast<size_t>(tiling.photoWidth) * tiling.photoHeight * 4)
    {
        throw std::runtime_error("a photo tile's picture or the canvas is smaller than the tiling says");
    }
    for (uint32_t row = 0; row < tile.height; ++row)
    {
        const size_t from = (static_cast<size_t>(tiling.guard + row) * view.width + tiling.guard) * 4;
        const size_t to = (static_cast<size_t>(tile.y + row) * tiling.photoWidth + tile.x) * 4;
        std::memcpy(canvas.data() + to, viewPixels.data() + from, static_cast<size_t>(tile.width) * 4);
    }
}

PhotoFraming ComputePhotoFraming(float viewportAspect, float photoAspect)
{
    PhotoFraming framing{};
    if (!(viewportAspect > 0.0f) || !(photoAspect > 0.0f))
    {
        return framing;
    }
    if (photoAspect >= viewportAspect)
    {
        // Wider than the viewport: its full width, a band of its height.
        framing.height = viewportAspect / photoAspect;
        framing.y = 0.5f * (1.0f - framing.height);
    }
    else
    {
        framing.width = photoAspect / viewportAspect;
        framing.x = 0.5f * (1.0f - framing.width);
    }
    return framing;
}

Camera PlacePhotoCamera(const Camera& viewport, float viewportAspect, float photoAspect)
{
    Camera camera = viewport;
    const PhotoFraming framing = ComputePhotoFraming(viewportAspect, photoAspect);
    // The projection keeps the vertical field of view; a band of the viewport's height is the same
    // view through a lens whose half-angle tangent is that share of the viewport's.
    const float halfTangent = std::tan(glm::radians(viewport.fovDegrees) * 0.5f) * framing.height;
    camera.fovDegrees = glm::degrees(2.0f * std::atan(halfTangent));
    camera.autoExposure.enabled = false;
    return camera;
}
}
