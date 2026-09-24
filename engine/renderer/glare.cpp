#include "glare.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me
{

float GlareFNumberFromEv100(float ev100)
{
    const float fNumber = std::sqrt(std::exp2(ev100) * kGlareShutterSeconds);
    return std::clamp(fNumber, kGlareMinFNumber, kGlareMaxFNumber);
}

std::vector<glm::vec3> ComputeGlareBands(float fNumber, uint32_t viewportHeight, size_t levelCount, float strength)
{
    std::vector<glm::vec3> bands(levelCount, glm::vec3(0.0f));
    if (levelCount == 0 || viewportHeight == 0 || !(strength > 0.0f))
    {
        return bands;
    }

    const float pitchMicrons = kGlareSensorHeightMicrons / static_cast<float>(viewportHeight);
    constexpr float kPiSquared = std::numbers::pi_v<float> * std::numbers::pi_v<float>;
    const glm::vec3 k = 2.0f * kGlareWavelengthsMicrons * fNumber / (kPiSquared * pitchMicrons) * strength;

    for (size_t level = 0; level < levelCount; ++level)
    {
        const float radius = std::exp2(static_cast<float>(level) + 1.0f);
        const bool last = level + 1 == levelCount;
        bands[level] = last ? k / radius : k * (1.0f / radius - 1.0f / (2.0f * radius));
    }

    // K / 2 grows with the viewport height; past kGlareMaxMovedEnergy the composite would take more
    // from a pixel than a lens could, so every band shrinks together.
    const float moved = k.r / 2.0f;
    if (moved > kGlareMaxMovedEnergy)
    {
        for (glm::vec3& band : bands)
        {
            band *= kGlareMaxMovedEnergy / moved;
        }
    }
    return bands;
}
}
