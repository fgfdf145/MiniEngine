#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace me
{

// Auto white balance as GT7 does it (docs/references/gt7-rendering-notes.md, section 4.4): estimate
// the scene's illuminant from its light references with a faint virtual D65 light, limit it to a
// camera's AWB range, adapt toward it over time, and balance partially, as the eye does.

inline constexpr glm::vec2 kD65WhiteXy{0.3127f, 0.3290f};
// A real camera's AWB range: warmer or cooler light is balanced only this far.
inline constexpr float kMinWhiteBalanceKelvin = 2800.0f;
inline constexpr float kMaxWhiteBalanceKelvin = 10000.0f;
// The virtual D65 light: this share of the measured illuminance plus a floor in lux, so a dark
// scene's estimate stays near neutral.
inline constexpr float kWhiteBalanceVirtualLightShare = 0.05f;
inline constexpr float kWhiteBalanceVirtualLightLux = 1.0f;

struct AutoWhiteBalanceSettings
{
    bool enabled = true;
    // Degree of chromatic adaptation: 1 balances fully to D65, 0 not at all.
    // Partial: the estimate is uncertain and the eye never fully discounts the light either.
    float degree = 0.6f;
    // Exponential adaptation rate of the white point, per second.
    float adaptPerSecond = 0.5f;
};

// Illuminance from the scene's lights, as linear Rec.709 whose luminance is lux. Left empty when the
// scene has none.
struct WhiteBalanceReferences
{
    std::optional<glm::vec3> sunIlluminanceRgb;
    std::optional<glm::vec3> skyIlluminanceRgb;
    // The view's average colour (any scale): what the camera actually sees lit. Lights the view
    // barely shows must not decide the balance alone.
    std::optional<glm::vec3> frameColorRgb;
};

// How much the view counts against the light references in the estimate, at equal luminance.
// The lights dominate: the view's average is surface colour times light, so a large coloured
// surface (a red wall, a lawn) would otherwise pass for a tinted illuminant and the balance would
// swing with the camera. The view still counts, so a light the camera barely sees (the sun on a far
// wall of an interior) does not decide the balance alone.
inline constexpr float kWhiteBalanceFrameWeight = 0.25f;

glm::vec3 Rec709ToXyz(const glm::vec3& rgb);
glm::vec3 XyzToRec709(const glm::vec3& xyz);
glm::vec2 XyzToXy(const glm::vec3& xyz);

// McCamy's cubic approximation, in kelvin.
float CorrelatedColorTemperature(const glm::vec2& xy);
// The Planckian locus (Kang et al. 2002), valid from 1667 K to 25000 K.
glm::vec2 PlanckianXy(float kelvin);
// Inside [kMinWhiteBalanceKelvin, kMaxWhiteBalanceKelvin] the white point is kept; outside, it moves
// to the Planckian locus at the nearer limit.
glm::vec2 LimitWhitePoint(const glm::vec2& xy);

// The luminance-weighted chromaticity of the light references and the virtual D65 light, blended
// at equal luminance with the view's colour (kWhiteBalanceFrameWeight), limited.
glm::vec2 EstimateIlluminantXy(const WhiteBalanceReferences& references);

// One step of exponential adaptation of the white point; frame-rate independent.
glm::vec2 AdaptWhitePointXy(const glm::vec2& current, const glm::vec2& target, float deltaSeconds, float ratePerSecond);

// Linear Rec.709 to linear Rec.709: a Bradford transform from whiteXy to D65 with the given degree
// of adaptation (cone responses scale by degree * D65 / white + 1 - degree).
glm::mat3 WhiteBalanceMatrix(const glm::vec2& whiteXy, float degree);
}
