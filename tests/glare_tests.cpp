#include <engine/renderer/glare.h>

#include <cmath>
#include <iostream>
#include <numbers>
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

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= 1e-5f * std::max(std::abs(lhs), std::abs(rhs));
}

// Energy beyond one pixel's radius, per channel: 2 lambda N / (pi^2 p).
glm::vec3 DiffractionScale(float fNumber, uint32_t viewportHeight)
{
    const float pitch = kGlareSensorHeightMicrons / static_cast<float>(viewportHeight);
    return 2.0f * kGlareWavelengthsMicrons * fNumber / (std::numbers::pi_v<float> * std::numbers::pi_v<float> * pitch);
}

void DaylightStopsDown()
{
    const float fNumber = GlareFNumberFromEv100(15.6f);
    Require(std::abs(fNumber - 20.1f) < 0.5f, "EV 15.6 at 1/125 s must be about f/20, got " + std::to_string(fNumber));
}

void FNumberIsClampedAndMonotonic()
{
    Require(GlareFNumberFromEv100(-2.0f) == kGlareMinFNumber, "a dark scene is shot wide open");
    Require(GlareFNumberFromEv100(20.0f) == kGlareMaxFNumber, "a very bright scene stops at the smallest aperture");
    float previous = 0.0f;
    for (float ev = -4.0f; ev <= 20.0f; ev += 0.25f)
    {
        const float fNumber = GlareFNumberFromEv100(ev);
        Require(std::isfinite(fNumber) && fNumber >= previous, "the f-number must never fall as the EV rises");
        previous = fNumber;
    }
}

void BandsSumToTheEnergyBeyondTwoPixels()
{
    const glm::vec3 k = DiffractionScale(8.0f, 541u);
    const std::vector<glm::vec3> bands = ComputeGlareBands(8.0f, 541u, 6, 1.0f);
    Require(bands.size() == 6, "one band per level");
    glm::vec3 sum(0.0f);
    for (const glm::vec3& band : bands)
    {
        sum += band;
    }
    for (int c = 0; c < 3; ++c)
    {
        Require(NearlyEqual(sum[c], k[c] / 2.0f), "the bands must move exactly the energy beyond 2 px");
    }
}

void BandsFollowTheAiryFalloff()
{
    const glm::vec3 k = DiffractionScale(8.0f, 541u);
    const std::vector<glm::vec3> bands = ComputeGlareBands(8.0f, 541u, 6, 1.0f);
    for (size_t level = 0; level + 1 < bands.size(); ++level)
    {
        // K (1/R - 1/2R) with R = 2^(level+1).
        Require(NearlyEqual(bands[level].g, k.g / std::exp2(static_cast<float>(level) + 2.0f)), "band " + std::to_string(level));
    }
    // The last level takes everything beyond its radius, K / R_last with R_last = 2^6.
    Require(NearlyEqual(bands.back().g, k.g / 64.0f), "the last band takes the far wing");
}

void RedSpreadsFurtherThanBlue()
{
    const std::vector<glm::vec3> bands = ComputeGlareBands(8.0f, 541u, 6, 1.0f);
    Require(bands[0].r > bands[0].g && bands[0].g > bands[0].b, "diffraction scales with wavelength");
}

void StrengthScalesAndZeroDisables()
{
    const std::vector<glm::vec3> one = ComputeGlareBands(8.0f, 541u, 6, 1.0f);
    const std::vector<glm::vec3> two = ComputeGlareBands(8.0f, 541u, 6, 2.0f);
    const std::vector<glm::vec3> zero = ComputeGlareBands(8.0f, 541u, 6, 0.0f);
    for (size_t level = 0; level < one.size(); ++level)
    {
        Require(NearlyEqual(two[level].g, 2.0f * one[level].g), "strength multiplies every band");
        Require(zero[level] == glm::vec3(0.0f), "zero strength moves nothing");
    }
}

void OneLevelTakesEverything()
{
    const glm::vec3 k = DiffractionScale(8.0f, 4u);
    const std::vector<glm::vec3> bands = ComputeGlareBands(8.0f, 4u, 1, 1.0f);
    Require(bands.size() == 1 && NearlyEqual(bands[0].g, k.g / 2.0f), "a one-level chain carries the whole wing");
}

void EnergyBeyondAnAngleIsResolutionIndependent()
{
    // Beyond 8 px at 541 px tall is the same angle as beyond 16 px at 1082: bands 2.. and 3...
    const std::vector<glm::vec3> low = ComputeGlareBands(8.0f, 541u, 6, 1.0f);
    const std::vector<glm::vec3> high = ComputeGlareBands(8.0f, 1082u, 7, 1.0f);
    float lowSum = 0.0f;
    float highSum = 0.0f;
    for (size_t level = 2; level < low.size(); ++level)
    {
        lowSum += low[level].g;
    }
    for (size_t level = 3; level < high.size(); ++level)
    {
        highSum += high[level].g;
    }
    Require(NearlyEqual(lowSum, highSum), "the glare must not depend on the render resolution");
}

void HdrDisplaysNeedLessGlare()
{
    // GT7 treats the display's peak above SDR's 250 nits as extra exposure latitude: a 1000-nit
    // display is two stops, so the aperture opens by two stops and the f-number halves.
    const float ev = 14.0f; // f/11.4 in SDR, clear of the f/22 clamp
    Require(GlareFNumberFromEv100(ev, kGlareSdrPeakNits) == GlareFNumberFromEv100(ev), "SDR is the default");
    const float sdr = GlareFNumberFromEv100(ev);
    const float hdr = GlareFNumberFromEv100(ev, 1000.0f);
    Require(std::abs(hdr - sdr / 2.0f) < 1e-3f, "a 1000-nit display halves the f-number: " + std::to_string(hdr));
    Require(GlareFNumberFromEv100(ev, 100.0f) == sdr, "a peak under SDR white never widens the glare");
}

void StaysEnergyConserving()
{
    // Even at the smallest aperture on a tall viewport, the moved energy must stay below 1.
    const std::vector<glm::vec3> bands = ComputeGlareBands(kGlareMaxFNumber, 4320u, 6, 4.0f);
    glm::vec3 sum(0.0f);
    for (const glm::vec3& band : bands)
    {
        sum += band;
    }
    Require(sum.r < 1.0f, "the composite must never move more energy than a pixel has");
}
}

int main()
{
    try
    {
        DaylightStopsDown();
        FNumberIsClampedAndMonotonic();
        BandsSumToTheEnergyBeyondTwoPixels();
        BandsFollowTheAiryFalloff();
        RedSpreadsFurtherThanBlue();
        StrengthScalesAndZeroDisables();
        OneLevelTakesEverything();
        EnergyBeyondAnAngleIsResolutionIndependent();
        StaysEnergyConserving();
        HdrDisplaysNeedLessGlare();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glare tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "glare tests passed\n";
    return 0;
}
