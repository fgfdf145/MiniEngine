#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace me
{

// Photometric exposure for a camera at ISO 100, in the saturation-based convention (K = 78 / q
// with q = 0.65, so the pixel that maps to 1.0 before tone mapping has luminance 1.2 * 2^EV100).
// Scene radiance is authored in physical units (lux, lumens, cd/m^2), so every radiance value is
// multiplied by this before the tone mapping operator sees it.
float ExposureFromEv100(float ev100);

// Physical radiance (cd/m^2) to the HDR target's unit: the scale every writer of the HDR target
// and GB3 applies. ExposureFromEv100(ev100) * kFrameBufferUnitsPerExposed.
float PreExposureFromEv100(float ev100);

// What TAA multiplies its history by: the history was written at historyPreExposure and this frame
// writes at currentPreExposure. 1 when there is no valid history or it carries no pre-exposure.
float TaaHistoryScale(bool historyValid, float currentPreExposure, float historyPreExposure);

// EV100 range the editor exposes. 16 is a sunlit exterior, 8 an overcast one or a bright interior,
// and 2 a dim interior lit by a few bulbs.
inline constexpr float kMinExposureEv100 = -2.0f;
inline constexpr float kMaxExposureEv100 = 18.0f;

// Where exposure starts before auto exposure has measured anything, and the manual default: a
// 1000 lx directional light on a white diffuse surface lands mid range after tone mapping.
inline constexpr float kDefaultExposureEv100 = 8.0f;

// The HDR target's unit (shaders/vulkan/pre_exposure.glsl): pre-exposed values, where 1.0 is
// 100 cd/m^2 as displayed, GT7's frame-buffer unit. An exposed value (1.0 = sensor saturation, see
// ExposureFromEv100) times this lands saturation on GT7's 250 cd/m^2 SDR paper white.
inline constexpr float kFrameBufferUnitsPerExposed = 2.5f;

// The editor viewport's background in HDR target units: what the tone mapping pass
// (TonemapFrameBufferRec709 in shaders/vulkan/gt7_tonemap.glsl) turns into the display-linear
// {0.08, 0.1, 0.16}. It stands for no physical light, so it is written as is at every exposure.
// Solved numerically against that operator; tests/tonemap_tests.cpp checks it still round-trips.
inline constexpr glm::vec3 kViewportBackgroundFrameBuffer{0.223550f, 0.272803f, 0.421025f};
inline constexpr glm::vec3 kViewportBackgroundDisplayLinear{0.08f, 0.1f, 0.16f};

// Number of bins exposure_histogram.comp writes; the binning itself is in
// shaders/vulkan/exposure_histogram.glsl, and exposure.cpp asserts the two agree.
inline constexpr uint32_t kExposureHistogramBinCount = 256;

struct AutoExposureSettings
{
    bool enabled = true;

    // Added on top of the metered exposure, in stops. Positive brightens the image.
    float compensationEv = 0.0f;

    // The metered EV100 is clamped into this range.
    float minEv100 = kMinExposureEv100;
    float maxEv100 = kMaxExposureEv100;

    // Only pixels between these fractions of the sorted histogram are averaged, so a dark corner
    // or a small, very bright light does not swing the exposure for the whole frame.
    float lowPercentile = 0.6f;
    float highPercentile = 0.95f;

    // Exponential adaptation rates, per second. The eye adapts to a brighter scene faster than to
    // a darker one, hence the two.
    float adaptToBrighterPerSecond = 3.0f;
    float adaptToDarkerPerSecond = 1.5f;
};

// Where a luminance lands in the histogram and where a bin sits, from the shared binning rule.
uint32_t ExposureHistogramBinForLuminance(float luminance);
float ExposureHistogramBinCenterLog2Luminance(uint32_t bin);

// The mean log2 luminance (cd/m^2) of the pixels between the two percentiles, from bin centers.
// Empty when the histogram counted no pixels, for example when nothing but background is visible.
std::optional<float> MeterAverageLog2Luminance(
    std::span<const uint32_t> histogram,
    float lowPercentile,
    float highPercentile);

// The EV100 a reflected-light meter (K = 12.5) reads for that average: it exposes the average to
// mid gray, 1 / 9.6 of sensor saturation.
float Ev100FromAverageLog2Luminance(float averageLog2Luminance);

// Meter, apply compensation and clamp: the EV100 auto exposure moves toward.
std::optional<float> MeterTargetEv100(std::span<const uint32_t> histogram, const AutoExposureSettings& settings);

// One step of exponential adaptation from current toward target. Frame-rate independent: two
// steps of dt / 2 land where one step of dt does.
float AdaptEv100(float currentEv100, float targetEv100, float deltaSeconds, const AutoExposureSettings& settings);
}
