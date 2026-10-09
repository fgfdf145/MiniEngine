#include <engine/editor/engine_settings.h>
#include <engine/editor/services/photo_mode.h>

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
