#include <engine/renderer/white_balance.h>

#include <cmath>
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

float MaxAbsDifference(const glm::mat3& a, const glm::mat3& b)
{
    float difference = 0.0f;
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            difference = std::max(difference, std::abs(a[column][row] - b[column][row]));
        }
    }
    return difference;
}

const glm::vec2 kIlluminantA{0.44757f, 0.40745f};

void D65IsLeftAlone()
{
    Require(MaxAbsDifference(WhiteBalanceMatrix(kD65WhiteXy, 1.0f), glm::mat3(1.0f)) < 1e-4f, "D65 needs no balance");
    Require(MaxAbsDifference(WhiteBalanceMatrix(kD65WhiteXy, 0.5f), glm::mat3(1.0f)) < 1e-4f, "at any degree");
}

void ZeroDegreeIsIdentity()
{
    Require(MaxAbsDifference(WhiteBalanceMatrix(kIlluminantA, 0.0f), glm::mat3(1.0f)) < 1e-5f, "degree 0 adapts nothing");
}

void FullAdaptationNeutralizesTheWhite()
{
    const glm::vec3 whiteXyz(kIlluminantA.x / kIlluminantA.y, 1.0f, (1.0f - kIlluminantA.x - kIlluminantA.y) / kIlluminantA.y);
    const glm::vec3 whiteRgb = XyzToRec709(whiteXyz);
    const glm::vec3 balanced = WhiteBalanceMatrix(kIlluminantA, 1.0f) * whiteRgb;
    Require(std::abs(balanced.r - balanced.g) < 1e-3f && std::abs(balanced.b - balanced.g) < 1e-3f, "the white must come out gray");
    const glm::vec3 partial = WhiteBalanceMatrix(kIlluminantA, 0.8f) * whiteRgb;
    Require(partial.r > partial.b, "partial adaptation leaves some warmth");
}

void ColorTemperatureOfStandardIlluminants()
{
    Require(std::abs(CorrelatedColorTemperature(kD65WhiteXy) - 6504.0f) < 20.0f, "D65 is about 6504 K");
    Require(std::abs(CorrelatedColorTemperature(kIlluminantA) - 2856.0f) < 20.0f, "A is about 2856 K");
    const glm::vec2 planck = PlanckianXy(6504.0f);
    // D65 is a daylight, 0.003 Duv above the blackbody locus: about 0.0054 in xy.
    Require(glm::length(planck - kD65WhiteXy) < 0.006f, "the Planckian locus passes near D65");
}

void LimitsFollowACameraAwbRange()
{
    const glm::vec2 candle = PlanckianXy(2000.0f);
    const glm::vec2 limited = LimitWhitePoint(candle);
    Require(std::abs(CorrelatedColorTemperature(limited) - kMinWhiteBalanceKelvin) < 30.0f, "2000 K clamps to the minimum");
    const glm::vec2 warm = PlanckianXy(5000.0f);
    Require(glm::length(LimitWhitePoint(warm) - warm) < 1e-6f, "5000 K is inside the range");
}

void EstimateFallsBackToTheVirtualLight()
{
    Require(glm::length(EstimateIlluminantXy(WhiteBalanceReferences{}) - kD65WhiteXy) < 1e-4f, "nothing measured reads D65");
}

void AWarmSunPullsTheEstimate()
{
    // A 3000 K sun of 50 000 lx, as linear Rec.709 scaled to its illuminance.
    const glm::vec2 sunXy = PlanckianXy(3000.0f);
    const glm::vec3 sunXyz(sunXy.x / sunXy.y, 1.0f, (1.0f - sunXy.x - sunXy.y) / sunXy.y);
    WhiteBalanceReferences references;
    references.sunIlluminanceRgb = XyzToRec709(sunXyz) * 50000.0f;
    const float kelvin = CorrelatedColorTemperature(EstimateIlluminantXy(references));
    Require(kelvin > 3000.0f && kelvin < 3300.0f, "the sun dominates, the 5% virtual D65 pulls slightly: " + std::to_string(kelvin));
}

void TheViewTempersTheLights()
{
    // A 3000 K sun the camera barely sees: the view reads neutral, so the estimate lands mostly on
    // the view (kWhiteBalanceFrameWeight, in XYZ at equal luminance), far from 3000 K.
    const glm::vec2 sunXy = PlanckianXy(3000.0f);
    const glm::vec3 sunXyz(sunXy.x / sunXy.y, 1.0f, (1.0f - sunXy.x - sunXy.y) / sunXy.y);
    WhiteBalanceReferences references;
    references.sunIlluminanceRgb = XyzToRec709(sunXyz) * 50000.0f;
    const float lightsOnly = CorrelatedColorTemperature(EstimateIlluminantXy(references));
    references.frameColorRgb = glm::vec3(1.0f);
    const float withView = CorrelatedColorTemperature(EstimateIlluminantXy(references));
    Require(withView > lightsOnly + 1000.0f && withView < 6000.0f, "a neutral view pulls the estimate toward D65: " + std::to_string(withView));
    // The view alone, with no light references, meets the virtual D65 light halfway.
    WhiteBalanceReferences viewOnly;
    viewOnly.frameColorRgb = glm::vec3(1.0f);
    Require(glm::length(EstimateIlluminantXy(viewOnly) - kD65WhiteXy) < 0.002f, "a gray view under no light reads about D65");
}

void AdaptationIsFrameRateIndependent()
{
    const glm::vec2 one = AdaptWhitePointXy(kD65WhiteXy, kIlluminantA, 1.0f, 0.5f);
    const glm::vec2 half = AdaptWhitePointXy(AdaptWhitePointXy(kD65WhiteXy, kIlluminantA, 0.5f, 0.5f), kIlluminantA, 0.5f, 0.5f);
    Require(glm::length(one - half) < 1e-5f, "two half steps land where one full step does");
    Require(glm::length(one - kD65WhiteXy) > 0.0f && glm::length(one - kIlluminantA) > 0.0f, "one second moves part way");
}
}

int main()
{
    try
    {
        D65IsLeftAlone();
        ZeroDegreeIsIdentity();
        FullAdaptationNeutralizesTheWhite();
        ColorTemperatureOfStandardIlluminants();
        LimitsFollowACameraAwbRange();
        EstimateFallsBackToTheVirtualLight();
        AWarmSunPullsTheEstimate();
        AdaptationIsFrameRateIndependent();
        TheViewTempersTheLights();
    }
    catch (const std::exception& error)
    {
        std::cerr << "white balance tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "white balance tests passed\n";
    return 0;
}
