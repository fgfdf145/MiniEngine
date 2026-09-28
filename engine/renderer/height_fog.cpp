#include "height_fog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

// (1 - exp(-x)) / x for x >= 0, the share of a segment's start density the whole segment averages.
// Below 1e-2 the series keeps its float precision, which the quotient loses as x goes to 0.
float SegmentAverage(float x)
{
    return x < 1e-2f ? 1.0f - x * 0.5f + x * x / 6.0f : (1.0f - std::exp(-x)) / x;
}
}

HeightFogSettings ClampHeightFogSettings(const HeightFogSettings& settings)
{
    HeightFogSettings clamped = settings;
    clamped.density = std::clamp(settings.density, 0.0f, 1.0f);
    clamped.heightFalloff = std::clamp(settings.heightFalloff, 1e-5f, 1.0f);
    clamped.startDistance = std::max(settings.startDistance, 0.0f);
    clamped.maxOpacity = std::clamp(settings.maxOpacity, 0.0f, 1.0f);
    clamped.albedo = glm::clamp(settings.albedo, glm::vec3(0.0f), glm::vec3(1.0f));
    clamped.anisotropy = std::clamp(settings.anisotropy, 0.0f, 0.95f);
    return clamped;
}

// With a0 the height exponent at the segment's start and a1 = a0 - k L at its end, the optical
// depth is density * exp(max(a0, a1)) * L * SegmentAverage(|k L|): factored at the denser end, so
// neither a camera high above the fog looking down nor one deep inside it looking up overflows or
// loses the fog to underflow.
float HeightFogOpticalDepth(const HeightFogSettings& settings, const glm::vec3& cameraPosition, const glm::vec3& direction, float distance)
{
    const float length = distance - settings.startDistance;
    if (settings.density <= 0.0f || length <= 0.0f)
    {
        return 0.0f;
    }
    const float startHeight = cameraPosition.y + settings.startDistance * direction.y;
    const float startExponent = -(startHeight - settings.fogHeight) * settings.heightFalloff;
    const float kL = settings.heightFalloff * direction.y * length;
    const float denserExponent = std::min(startExponent + std::max(-kL, 0.0f), kHeightFogMaxExponent);
    return settings.density * std::exp(denserExponent) * length * SegmentAverage(std::abs(kL));
}

float HeightFogSkyOpticalDepth(const HeightFogSettings& settings, const glm::vec3& cameraPosition, const glm::vec3& direction)
{
    if (settings.density <= 0.0f)
    {
        return 0.0f;
    }
    if (direction.y <= 0.0f)
    {
        return std::numeric_limits<float>::infinity();
    }
    const float startHeight = cameraPosition.y + settings.startDistance * direction.y;
    const float startExponent = std::min(-(startHeight - settings.fogHeight) * settings.heightFalloff, kHeightFogMaxExponent);
    return settings.density * std::exp(startExponent) / (settings.heightFalloff * direction.y);
}

float HenyeyGreenstein(float g, float cosTheta)
{
    const float denominator = std::max(1.0f + g * g - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - g * g) / (4.0f * kPi * denominator * std::sqrt(denominator));
}
}
