#include "exposure.h"

#include <algorithm>
#include <cmath>

// The binning rule, compiled from the same source the compute shader includes.
namespace me::exposure_shader
{
using namespace glm;
using uint = unsigned int;
#include <shaders/vulkan/exposure_histogram.glsl>
}

namespace me
{

static_assert(
    kExposureHistogramBinCount == exposure_shader::kExposureHistogramBinCount,
    "the C++ bin count must match exposure_histogram.glsl");

float ExposureFromEv100(float ev100)
{
    return 1.0f / (1.2f * std::exp2(ev100));
}

float PreExposureFromEv100(float ev100)
{
    return ExposureFromEv100(ev100) * kFrameBufferUnitsPerExposed;
}

float TaaHistoryScale(bool historyValid, float currentPreExposure, float historyPreExposure)
{
    if (!historyValid || !(historyPreExposure > 0.0f))
    {
        return 1.0f;
    }
    return currentPreExposure / historyPreExposure;
}

uint32_t ExposureHistogramBinForLuminance(float luminance)
{
    return exposure_shader::ExposureHistogramBin(luminance);
}

float ExposureHistogramBinCenterLog2Luminance(uint32_t bin)
{
    return exposure_shader::ExposureHistogramBinCenterLog2(bin);
}

std::optional<float> MeterAverageLog2Luminance(
    std::span<const uint32_t> histogram,
    float lowPercentile,
    float highPercentile)
{
    uint64_t total = 0;
    for (const uint32_t count : histogram)
    {
        total += count;
    }
    if (total == 0)
    {
        return std::nullopt;
    }

    const double low = static_cast<double>(std::clamp(lowPercentile, 0.0f, 1.0f)) * static_cast<double>(total);
    const double high = static_cast<double>(std::clamp(highPercentile, 0.0f, 1.0f)) * static_cast<double>(total);

    // Each bin contributes the part of its pixel range that falls inside [low, high).
    double weightedSum = 0.0;
    double weight = 0.0;
    double binStart = 0.0;
    for (size_t bin = 0; bin < histogram.size(); ++bin)
    {
        const double binEnd = binStart + static_cast<double>(histogram[bin]);
        const double overlap = std::min(binEnd, high) - std::max(binStart, low);
        if (overlap > 0.0)
        {
            weightedSum += overlap * ExposureHistogramBinCenterLog2Luminance(static_cast<uint32_t>(bin));
            weight += overlap;
        }
        binStart = binEnd;
    }

    // A window with no width, low == high, still has to meter something: fall back to the bin the
    // low percentile lands in.
    if (weight <= 0.0)
    {
        binStart = 0.0;
        for (size_t bin = 0; bin < histogram.size(); ++bin)
        {
            binStart += static_cast<double>(histogram[bin]);
            if (binStart > low)
            {
                return ExposureHistogramBinCenterLog2Luminance(static_cast<uint32_t>(bin));
            }
        }
        return ExposureHistogramBinCenterLog2Luminance(static_cast<uint32_t>(histogram.size() - 1));
    }

    return static_cast<float>(weightedSum / weight);
}

float Ev100FromAverageLog2Luminance(float averageLog2Luminance)
{
    // EV100 = log2(L * S / K) with S = 100 and K = 12.5, and log2(100 / 12.5) = 3.
    return averageLog2Luminance + 3.0f;
}

std::optional<float> MeterTargetEv100(std::span<const uint32_t> histogram, const AutoExposureSettings& settings)
{
    const std::optional<float> averageLog2Luminance =
        MeterAverageLog2Luminance(histogram, settings.lowPercentile, settings.highPercentile);
    if (!averageLog2Luminance.has_value())
    {
        return std::nullopt;
    }

    // A lower EV100 is a longer exposure, so brightening by n stops subtracts n.
    const float ev100 = Ev100FromAverageLog2Luminance(*averageLog2Luminance) - settings.compensationEv;
    const float lower = std::min(settings.minEv100, settings.maxEv100);
    const float upper = std::max(settings.minEv100, settings.maxEv100);
    return std::clamp(ev100, lower, upper);
}

float AdaptEv100(float currentEv100, float targetEv100, float deltaSeconds, const AutoExposureSettings& settings)
{
    if (!(deltaSeconds > 0.0f))
    {
        return currentEv100;
    }

    // A rising EV100 means the scene got brighter.
    const float rate = targetEv100 > currentEv100 ? settings.adaptToBrighterPerSecond : settings.adaptToDarkerPerSecond;
    const float blend = 1.0f - std::exp(-std::max(rate, 0.0f) * deltaSeconds);
    return currentEv100 + (targetEv100 - currentEv100) * blend;
}
}
