#include "exposure.h"

#include <algorithm>
#include <cmath>
#include <numbers>

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

namespace
{
float AdaptAtRates(float currentEv100, float targetEv100, float deltaSeconds, float toBrighter, float toDarker)
{
    if (!(deltaSeconds > 0.0f))
    {
        return currentEv100;
    }

    // A rising EV100 means the scene got brighter.
    const float rate = targetEv100 > currentEv100 ? toBrighter : toDarker;
    const float blend = 1.0f - std::exp(-std::max(rate, 0.0f) * deltaSeconds);
    return currentEv100 + (targetEv100 - currentEv100) * blend;
}
}

float AdaptEv100(float currentEv100, float targetEv100, float deltaSeconds, const AutoExposureSettings& settings)
{
    return AdaptAtRates(
        currentEv100, targetEv100, deltaSeconds, settings.adaptToBrighterPerSecond, settings.adaptToDarkerPerSecond);
}

std::optional<float> MeterLongTermTargetEv100(const ExposureReferences& references, const AutoExposureSettings& settings)
{
    double weightedSum = 0.0;
    double weight = 0.0;
    const auto add = [&](float log2Luminance, float referenceWeight)
    {
        if (referenceWeight > 0.0f && std::isfinite(log2Luminance))
        {
            weightedSum += static_cast<double>(referenceWeight) * Ev100FromAverageLog2Luminance(log2Luminance);
            weight += referenceWeight;
        }
    };

    if (references.frameLog2Luminance.has_value())
    {
        add(*references.frameLog2Luminance, settings.frameReferenceWeight);
    }
    // A reflected-light meter aimed at an 18% gray card under the sun.
    constexpr float kMinSunLux = 0.001f;
    if (references.sunIlluminanceLux.has_value() && *references.sunIlluminanceLux >= kMinSunLux)
    {
        add(std::log2(*references.sunIlluminanceLux * 0.18f / std::numbers::pi_v<float>), settings.sunReferenceWeight);
    }
    if (references.skyLuminance.has_value() && *references.skyLuminance > 0.0f)
    {
        add(std::log2(*references.skyLuminance), settings.skyReferenceWeight);
    }
    if (weight <= 0.0)
    {
        return std::nullopt;
    }

    const float ev100 = static_cast<float>(weightedSum / weight) - settings.compensationEv;
    const float lower = std::min(settings.minEv100, settings.maxEv100);
    const float upper = std::max(settings.minEv100, settings.maxEv100);
    return std::clamp(ev100, lower, upper);
}

float StepAutoExposure(
    AutoExposureState& state,
    float currentEv100,
    float frameTargetEv100,
    std::optional<float> longTermTargetEv100,
    float deltaSeconds,
    const AutoExposureSettings& settings)
{
    const float range = std::max(settings.shortTermRangeEv, 0.0f);
    if (!state.initialized)
    {
        state.longTermEv100 = longTermTargetEv100.value_or(frameTargetEv100);
        state.initialized = true;
        return std::clamp(frameTargetEv100, state.longTermEv100 - range, state.longTermEv100 + range);
    }

    if (longTermTargetEv100.has_value())
    {
        state.longTermEv100 = AdaptAtRates(
            state.longTermEv100,
            *longTermTargetEv100,
            deltaSeconds,
            settings.longTermToBrighterPerSecond,
            settings.longTermToDarkerPerSecond);
    }
    const float shortTermTarget =
        std::clamp(frameTargetEv100, state.longTermEv100 - range, state.longTermEv100 + range);
    return AdaptEv100(currentEv100, shortTermTarget, deltaSeconds, settings);
}
}
