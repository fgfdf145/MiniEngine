#include "photo_mode.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

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
