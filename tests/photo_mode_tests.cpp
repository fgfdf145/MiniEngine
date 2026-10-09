#include <engine/editor/engine_settings.h>
#include <engine/editor/services/photo_mode.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

// A wider photo spans the viewport's width in a band of its height; a taller one its height in a
// column of its width; the same aspect all of it. Always centred.
void FramesInsideTheViewport()
{
    const PhotoFraming wide = ComputePhotoFraming(4.0f / 3.0f, 16.0f / 9.0f);
    Require(Near(wide.x, 0.0f) && Near(wide.width, 1.0f), "a 16:9 photo spans a 4:3 viewport's width");
    Require(Near(wide.height, 0.75f) && Near(wide.y, 0.125f), "in a centred band three quarters of its height");

    const PhotoFraming tall = ComputePhotoFraming(16.0f / 9.0f, 1.0f);
    Require(Near(tall.y, 0.0f) && Near(tall.height, 1.0f), "a square photo spans a 16:9 viewport's height");
    Require(Near(tall.width, 9.0f / 16.0f) && Near(tall.x, 0.5f * (1.0f - 9.0f / 16.0f)), "in a centred column");

    const PhotoFraming same = ComputePhotoFraming(1.5f, 1.5f);
    Require(Near(same.x, 0.0f) && Near(same.y, 0.0f) && Near(same.width, 1.0f) && Near(same.height, 1.0f),
            "the viewport's own aspect is the whole viewport");

    const PhotoFraming invalid = ComputePhotoFraming(0.0f, 1.0f);
    Require(Near(invalid.width, 1.0f) && Near(invalid.height, 1.0f), "no aspect falls back to the whole viewport");
}

// The photo's camera sees exactly the framed part of the viewport: its vertical half-angle tangent
// is the band's share of the viewport's, its horizontal one then the viewport's. It holds the
// viewport's exposure.
void CameraSeesTheFramedPart()
{
    Camera viewport;
    viewport.fovDegrees = 47.0f;
    viewport.autoExposure.enabled = true;
    viewport.exposureEv100 = 8.3f;
    const float viewportAspect = 4.0f / 3.0f;
    const float photoAspect = 16.0f / 9.0f;
    const Camera photo = PlacePhotoCamera(viewport, viewportAspect, photoAspect);

    const float viewportHalfTangent = std::tan(glm::radians(viewport.fovDegrees) * 0.5f);
    const float photoHalfTangent = std::tan(glm::radians(photo.fovDegrees) * 0.5f);
    Require(Near(photoHalfTangent, viewportHalfTangent * 0.75f), "a 16:9 photo of a 4:3 viewport narrows the vertical view to the band");
    Require(Near(photoHalfTangent * photoAspect, viewportHalfTangent * viewportAspect), "and keeps the horizontal view");
    Require(!photo.autoExposure.enabled && Near(photo.exposureEv100, 8.3f), "at the viewport's exposure, held");
    Require(photo.position == viewport.position && Near(photo.yawDegrees, viewport.yawDegrees), "from the viewport's place");

    const Camera narrow = PlacePhotoCamera(viewport, viewportAspect, 1.0f);
    Require(Near(narrow.fovDegrees, viewport.fovDegrees, 1e-3f), "a taller photo keeps the vertical view");
}

void ClampsToWhatTheWindowOffers()
{
    PhotoModeSettings settings;
    settings.width = 20000;
    settings.height = 1;
    settings.warmupFrames = 0;
    settings = ClampPhotoModeSettings(settings);
    Require(settings.width == kPhotoMaxSize && settings.height == kPhotoMinSize, "the size stays within the limits");
    Require(settings.warmupFrames == kPhotoMinWarmupFrames, "at least one frame renders");
}

// Needed: the viewport's bytes per pixel times the photo's pixels; free: the budget less the usage.
void EstimatesGpuMemory()
{
    GpuMemoryReport memory;
    Require(!EstimatePhotoMemory(memory, 3840, 2160).has_value(), "nothing before the first measurement");
    memory.serial = 10;
    memory.budget = uint64_t{8} << 30;
    memory.usage = uint64_t{5} << 30;
    memory.viewBytesPerPixel = 300.0;
    const std::optional<PhotoMemoryEstimate> fourK = EstimatePhotoMemory(memory, 3840, 2160);
    Require(fourK.has_value() && fourK->neededBytes == uint64_t{300} * 3840 * 2160, "4K needs 300 bytes a pixel");
    Require(fourK->freeBytes == uint64_t{3} << 30 && fourK->Fits(), "and fits in 3 GB");
    Require(!EstimatePhotoMemory(memory, 7680, 4320)->Fits(), "8K does not");
    memory.usage = memory.budget + 1;
    Require(EstimatePhotoMemory(memory, 64, 64)->freeBytes == 0, "over budget leaves nothing free");
}

// A photo within one view's pixels is one tile of its own size with no guard; a larger one the
// fewest even tiles of a side within the view's square, each with the guard, covering the photo
// once, the last row and column clipped to it.
void PlansTiles()
{
    const PhotoTiling whole = PlanPhotoTiles(3840, 2160, kPhotoMaxViewPixels);
    Require(!whole.Tiled() && whole.tiles.size() == 1 && whole.guard == 0, "4K is one view");
    Require(whole.ViewExtent().width == 3840 && whole.ViewExtent().height == 2160, "at its own size");

    const PhotoTiling eightK = PlanPhotoTiles(7680, 4320, kPhotoMaxViewPixels);
    Require(eightK.Tiled() && eightK.guard == kPhotoTileGuard, "8K is tiled, with the guard");
    Require(eightK.columns == 4 && eightK.rows == 3 && eightK.tileWidth == 1920 && eightK.tileHeight == 1440,
            "in 4 x 3 tiles of 1920 x 1440 (sides of at most 2048)");
    Require(eightK.ViewExtent().width == 1920 + 2 * kPhotoTileGuard, "each view the tile and its guard");

    const PhotoTiling odd = PlanPhotoTiles(5001, 3001, 1000000);
    uint64_t covered = 0;
    for (const PhotoTile& tile : odd.tiles)
    {
        Require(tile.x + tile.width <= 5001 && tile.y + tile.height <= 3001, "no tile reaches past the photo");
        Require(tile.width <= odd.tileWidth && tile.height <= odd.tileHeight, "nor is larger than the tiling's");
        covered += static_cast<uint64_t>(tile.width) * tile.height;
    }
    Require(covered == uint64_t{5001} * 3001, "the tiles cover the photo exactly once");
    const RenderExtent view = odd.ViewExtent();
    Require(static_cast<uint64_t>(view.width) * view.height <= uint64_t{1100} * 1100, "each view about within the pixels given");

    const PhotoTiling tiny = PlanPhotoTiles(8192, 8192, 1000);
    Require(tiny.tileWidth <= kPhotoMinTileSide && tiny.tileWidth * tiny.columns >= 8192, "too little memory still makes tiles of the smallest side");
}

// What lands on a photo pixel lands on the same pixel of the tile's view, offset by the tile's corner
// less its guard: through the whole photo's projection and the tile's, Y up and inverted.
void TileProjectionMatchesTheWhole()
{
    const PhotoTiling tiling = PlanPhotoTiles(3000, 2000, 1000000);
    Require(tiling.Tiled(), "the test photo is tiled");
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(40.0f), 1.5f, 0.1f, 1000.0f);
    const RenderExtent view = tiling.ViewExtent();
    for (const bool inverted : {false, true})
    {
        glm::mat4 whole = projection;
        if (inverted)
        {
            whole[1][1] *= -1.0f;
        }
        for (const PhotoTile& tile : tiling.tiles)
        {
            const glm::mat4 narrowed = TileProjection(whole, tiling, tile, inverted);
            for (const glm::vec3 point : {glm::vec3(0.3f, -0.2f, -5.0f), glm::vec3(-1.4f, 0.9f, -3.0f), glm::vec3(4.0f, 2.5f, -12.0f)})
            {
                const glm::vec4 a = whole * glm::vec4(point, 1.0f);
                const glm::vec4 b = narrowed * glm::vec4(point, 1.0f);
                Require(Near(a.w, b.w, 1e-5f) && Near(a.z, b.z, 1e-5f), "a tile keeps the depth");
                // Pixels from the top-left: NDC Y up for the plain projection, down for the inverted one.
                const auto pixel = [&](const glm::vec4& clip, float width, float height)
                {
                    const float x = (clip.x / clip.w + 1.0f) * 0.5f * width;
                    const float yNdc = clip.y / clip.w;
                    const float y = (inverted ? yNdc + 1.0f : 1.0f - yNdc) * 0.5f * height;
                    return glm::vec2(x, y);
                };
                const glm::vec2 inPhoto = pixel(a, static_cast<float>(tiling.photoWidth), static_cast<float>(tiling.photoHeight));
                const glm::vec2 inView = pixel(b, static_cast<float>(view.width), static_cast<float>(view.height));
                const glm::vec2 corner(static_cast<float>(tile.x) - static_cast<float>(tiling.guard), static_cast<float>(tile.y) - static_cast<float>(tiling.guard));
                Require(glm::length(inView - (inPhoto - corner)) < 0.01f, inverted ? "the inverted tile lines up with the whole" : "the tile lines up with the whole");
            }
        }
    }
}

// Each tile's part of its view, without the guard, lands where the tile is on the canvas.
void CopiesTilesIntoTheCanvas()
{
    const PhotoTiling tiling = PlanPhotoTiles(1300, 900, 300000);
    Require(tiling.Tiled(), "the test photo is tiled");
    std::vector<uint8_t> canvas(static_cast<size_t>(1300) * 900 * 4, 0);
    const RenderExtent view = tiling.ViewExtent();
    for (size_t index = 0; index < tiling.tiles.size(); ++index)
    {
        const PhotoTile& tile = tiling.tiles[index];
        // Each view pixel holds where in the photo it shows, and the tile's number.
        std::vector<uint8_t> pixels(static_cast<size_t>(view.width) * view.height * 4);
        for (uint32_t y = 0; y < view.height; ++y)
        {
            for (uint32_t x = 0; x < view.width; ++x)
            {
                const int photoX = static_cast<int>(tile.x + x) - static_cast<int>(tiling.guard);
                const int photoY = static_cast<int>(tile.y + y) - static_cast<int>(tiling.guard);
                uint8_t* texel = pixels.data() + (static_cast<size_t>(y) * view.width + x) * 4;
                texel[0] = static_cast<uint8_t>(photoX & 0xFF);
                texel[1] = static_cast<uint8_t>(photoY & 0xFF);
                texel[2] = static_cast<uint8_t>(index);
                texel[3] = 255;
            }
        }
        CopyTileIntoCanvas(tiling, tile, pixels, canvas);
    }
    for (uint32_t y = 0; y < 900; y += 7)
    {
        for (uint32_t x = 0; x < 1300; x += 5)
        {
            const uint8_t* texel = canvas.data() + (static_cast<size_t>(y) * 1300 + x) * 4;
            Require(texel[0] == (x & 0xFF) && texel[1] == (y & 0xFF) && texel[3] == 255, "every canvas pixel comes from its place in a tile");
            const uint32_t column = x / tiling.tileWidth;
            const uint32_t row = y / tiling.tileHeight;
            Require(texel[2] == row * tiling.columns + column, "from the tile that holds it");
        }
    }
}

// The view's pixels follow the free memory, at most 4K's.
void LimitsViewsToFreeMemory()
{
    GpuMemoryReport memory;
    Require(PhotoMaxViewPixels(memory) == kPhotoMaxViewPixels, "unmeasured: 4K");
    memory.serial = 10;
    memory.budget = uint64_t{8} << 30;
    memory.usage = uint64_t{6} << 30;
    memory.viewBytesPerPixel = 400.0;
    const uint64_t pixels = PhotoMaxViewPixels(memory);
    Require(pixels < kPhotoMaxViewPixels && static_cast<double>(pixels) * 400.0 <= 0.75 * static_cast<double>(uint64_t{2} << 30) + 400.0,
            "2 GB free holds three quarters of it in pixels");
    memory.usage = 0;
    Require(PhotoMaxViewPixels(memory) == kPhotoMaxViewPixels, "plenty free: still no more than 4K");
}

void SettingsRoundTrip()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "miniengine_photo_mode_settings.json";
    EngineSettings saved;
    saved.photoMode.width = 2160;
    saved.photoMode.height = 2700;
    saved.photoMode.warmupFrames = 64;
    saved.photoMode.framingGuide = false;
    std::string error;
    Require(SaveEngineSettings(path, saved, error), "the settings save: " + error);
    EngineSettings loaded;
    Require(LoadEngineSettings(path, loaded, error), "the settings load: " + error);
    Require(loaded.photoMode == saved.photoMode, "the photo mode settings come back");

    std::ofstream(path) << "{ \"version\": 1 }";
    EngineSettings old;
    Require(LoadEngineSettings(path, old, error), "the old settings load: " + error);
    std::filesystem::remove(path);
    Require(old.photoMode == PhotoModeSettings{}, "without photo mode settings the defaults apply");
}
}

int main()
{
    try
    {
        FramesInsideTheViewport();
        CameraSeesTheFramedPart();
        ClampsToWhatTheWindowOffers();
        EstimatesGpuMemory();
        PlansTiles();
        TileProjectionMatchesTheWhole();
        CopiesTilesIntoTheCanvas();
        LimitsViewsToFreeMemory();
        SettingsRoundTrip();
    }
    catch (const std::exception& error)
    {
        std::cerr << "photo mode tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "photo mode tests passed\n";
    return 0;
}
