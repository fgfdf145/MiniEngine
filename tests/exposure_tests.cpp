#include <engine/renderer/camera.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Relative, because exposures span six orders of magnitude across the EV range.
bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= 1e-5f * std::abs(rhs);
}

void ZeroEvMapsLuminancePointTwoToOne()
{
    // At EV100 0 the saturation-based convention puts 1.2 cd/m^2 at the top of the range.
    Require(NearlyEqual(ExposureFromEv100(0.0f), 1.0f / 1.2f), "EV100 0 must give an exposure of 1 / 1.2");
}

void EachStopHalvesTheExposure()
{
    for (float ev = kMinExposureEv100; ev < kMaxExposureEv100; ev += 1.0f)
    {
        Require(
            NearlyEqual(ExposureFromEv100(ev + 1.0f), ExposureFromEv100(ev) * 0.5f),
            "one EV stop must halve the exposure");
    }
}

void CameraUsesItsOwnEv()
{
    Camera camera;
    Require(NearlyEqual(camera.exposureEv100, kDefaultExposureEv100), "a new camera must start at the default EV100");
    camera.exposureEv100 = 3.0f;
    Require(NearlyEqual(camera.GetExposure(), ExposureFromEv100(3.0f)), "GetExposure must follow exposureEv100");
}

void DefaultEvExposesTheDefaultSunMidRange()
{
    // A white Lambertian surface under the default 1000 lx directional light has radiance
    // 1000 / pi. The default EV exists to keep that below the point where the tone mapping
    // operator reaches display white (about 2 on the exposed scale), where it sat, far past,
    // before exposure existed.
    const float radiance = 1000.0f / std::numbers::pi_v<float>;
    const float preExposed = radiance * ExposureFromEv100(kDefaultExposureEv100);
    Require(preExposed > 0.25f && preExposed < 2.0f, "the default EV must expose a 1000 lx white surface below white");
}

void BackgroundClearStaysInsideFp16()
{
    // The forward pass clears the HDR target to background / exposure so the editor background
    // does not move with the slider. The brightest channel must stay representable at the top EV.
    const float brightestBackgroundChannel =
        std::max({kViewportBackgroundExposed.x, kViewportBackgroundExposed.y, kViewportBackgroundExposed.z});
    Require(
        brightestBackgroundChannel / ExposureFromEv100(kMaxExposureEv100) < 65504.0f,
        "the background clear must stay below fp16's maximum at the highest EV");
}

// ---------------------------------------------------------------------------
// Auto exposure
// ---------------------------------------------------------------------------

using Histogram = std::array<uint32_t, kExposureHistogramBinCount>;

float BinWidthLog2()
{
    return ExposureHistogramBinCenterLog2Luminance(1) - ExposureHistogramBinCenterLog2Luminance(0);
}

void BinsCoverTheirLuminance()
{
    for (float log2Luminance = -11.0f; log2Luminance <= 17.0f; log2Luminance += 0.37f)
    {
        const uint32_t bin = ExposureHistogramBinForLuminance(std::exp2(log2Luminance));
        Require(
            std::abs(ExposureHistogramBinCenterLog2Luminance(bin) - log2Luminance) <= 0.5f * BinWidthLog2() + 1e-4f,
            "a luminance must land in the bin whose center is within half a bin of it");
    }

    Require(ExposureHistogramBinForLuminance(0.0f) == 0, "black must land in the first bin");
    Require(ExposureHistogramBinForLuminance(1e-9f) == 0, "luminance below the range must clamp to the first bin");
    Require(
        ExposureHistogramBinForLuminance(1e12f) == kExposureHistogramBinCount - 1,
        "luminance above the range must clamp to the last bin");
}

void EmptyHistogramMetersNothing()
{
    const Histogram histogram{};
    Require(!MeterTargetEv100(histogram, AutoExposureSettings{}).has_value(), "an empty histogram must not produce an exposure");
}

void UniformSceneIsExposedToMidGray()
{
    // Every pixel at 50 cd/m^2: the metered exposure must put that at mid gray, 1 / 9.6 of sensor
    // saturation, give or take half a bin.
    const float luminance = 50.0f;
    Histogram histogram{};
    histogram[ExposureHistogramBinForLuminance(luminance)] = 10000;

    const std::optional<float> ev100 = MeterTargetEv100(histogram, AutoExposureSettings{});
    Require(ev100.has_value(), "a non-empty histogram must meter");
    const float exposedStops = std::log2(luminance * ExposureFromEv100(*ev100) * 9.6f);
    Require(std::abs(exposedStops) <= 0.5f * BinWidthLog2() + 1e-4f, "a uniform scene must expose to mid gray");
}

void SmallBrightAreaDoesNotSwingTheExposure()
{
    // 97% of the frame at 10 cd/m^2 and 3% at 100000: the bright 3% is above the 95th percentile,
    // so the meter must read the 10 cd/m^2 majority.
    Histogram histogram{};
    histogram[ExposureHistogramBinForLuminance(10.0f)] = 9700;
    histogram[ExposureHistogramBinForLuminance(100000.0f)] = 300;

    const std::optional<float> average = MeterAverageLog2Luminance(histogram, 0.6f, 0.95f);
    Require(average.has_value(), "the histogram must meter");
    Require(std::abs(*average - std::log2(10.0f)) <= 0.5f * BinWidthLog2() + 1e-4f, "a small bright area must be ignored");
}

void PercentileWindowSplitsBins()
{
    // Half the pixels in one bin, half in another four stops brighter. The 25%-75% window takes
    // half of each, so the average must sit midway between the two bin centers.
    Histogram histogram{};
    const uint32_t darkBin = ExposureHistogramBinForLuminance(1.0f);
    const uint32_t brightBin = ExposureHistogramBinForLuminance(16.0f);
    histogram[darkBin] = 500;
    histogram[brightBin] = 500;

    const std::optional<float> average = MeterAverageLog2Luminance(histogram, 0.25f, 0.75f);
    const float expected = 0.5f * (ExposureHistogramBinCenterLog2Luminance(darkBin) + ExposureHistogramBinCenterLog2Luminance(brightBin));
    Require(average.has_value() && std::abs(*average - expected) <= 1e-4f, "the window must weight each bin by its overlap");
}

void CompensationAndRangeApply()
{
    Histogram histogram{};
    histogram[ExposureHistogramBinForLuminance(50.0f)] = 1000;

    AutoExposureSettings settings{};
    const float metered = *MeterTargetEv100(histogram, settings);

    settings.compensationEv = 1.5f;
    Require(NearlyEqual(*MeterTargetEv100(histogram, settings), metered - 1.5f), "positive compensation must lower the EV100");

    settings.compensationEv = 0.0f;
    settings.minEv100 = metered + 2.0f;
    settings.maxEv100 = metered + 4.0f;
    Require(NearlyEqual(*MeterTargetEv100(histogram, settings), metered + 2.0f), "the metered EV100 must clamp into the range");
}

void AdaptationIsFrameRateIndependent()
{
    const AutoExposureSettings settings{};
    for (const float target : {12.0f, 4.0f})
    {
        const float oneStep = AdaptEv100(8.0f, target, 0.1f, settings);
        const float twoSteps = AdaptEv100(AdaptEv100(8.0f, target, 0.05f, settings), target, 0.05f, settings);
        Require(std::abs(oneStep - twoSteps) <= 1e-4f, "two half steps must land where one full step does");
    }
}

void AdaptationMovesTowardTargetAtItsRate()
{
    const AutoExposureSettings settings{};

    Require(NearlyEqual(AdaptEv100(8.0f, 12.0f, 0.0f, settings), 8.0f), "no elapsed time must not move the exposure");

    // After one time constant, 1 - 1/e of the way there.
    const float towardBrighter = AdaptEv100(8.0f, 12.0f, 1.0f / settings.adaptToBrighterPerSecond, settings);
    Require(std::abs(towardBrighter - (8.0f + 4.0f * (1.0f - std::exp(-1.0f)))) <= 1e-4f, "a brighter scene must adapt at its rate");
    const float towardDarker = AdaptEv100(8.0f, 4.0f, 1.0f / settings.adaptToDarkerPerSecond, settings);
    Require(std::abs(towardDarker - (8.0f - 4.0f * (1.0f - std::exp(-1.0f)))) <= 1e-4f, "a darker scene must adapt at its rate");

    float ev100 = 8.0f;
    for (int frame = 0; frame < 600; ++frame)
    {
        ev100 = AdaptEv100(ev100, 12.0f, 1.0f / 60.0f, settings);
    }
    Require(std::abs(ev100 - 12.0f) <= 1e-3f, "ten seconds must be enough to converge");
}
}

int main()
{
    try
    {
        ZeroEvMapsLuminancePointTwoToOne();
        EachStopHalvesTheExposure();
        CameraUsesItsOwnEv();
        DefaultEvExposesTheDefaultSunMidRange();
        BackgroundClearStaysInsideFp16();
        BinsCoverTheirLuminance();
        EmptyHistogramMetersNothing();
        UniformSceneIsExposedToMidGray();
        SmallBrightAreaDoesNotSwingTheExposure();
        PercentileWindowSplitsBins();
        CompensationAndRangeApply();
        AdaptationIsFrameRateIndependent();
        AdaptationMovesTowardTargetAtItsRate();
    }
    catch (const std::exception& error)
    {
        std::cerr << "exposure tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "exposure tests passed\n";
    return 0;
}
