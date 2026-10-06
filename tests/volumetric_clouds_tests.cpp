#include <engine/renderer/volumetric_clouds.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kPlanet = 6360.0f;
constexpr float kInner = kPlanet + 1.5f;
constexpr float kOuter = kInner + 2.5f;
constexpr float kMaxDistance = 1000.0f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Near(float a, float b, float tolerance)
{
    return std::abs(a - b) <= tolerance;
}

void ClampsSettings()
{
    CloudSettings wild{};
    wild.coverage = 2.0f;
    wild.baseAltitude = -5.0f;
    wild.thickness = 1e6f;
    wild.density = 0.0f;
    wild.shapeScale = 1.0f;
    wild.detailScale = 1e9f;
    wild.weatherScale = 0.0f;
    wild.billows = -1.0f;
    wild.forwardAnisotropy = 1.0f;
    wild.backAnisotropy = 0.5f;
    wild.backWeight = 3.0f;
    wild.albedo = 1.5f;
    wild.ambientScale = -1.0f;
    wild.hazeDistance = 0.0f;
    wild.diffusion = 2.0f;
    wild.ambientOcclusion = -1.0f;
    const CloudSettings clamped = ClampCloudSettings(wild);
    Require(clamped.coverage == 1.0f && clamped.baseAltitude == 100.0f && clamped.thickness == 10000.0f, "coverage and layer clamped");
    Require(clamped.density == 0.001f, "density clamped above zero");
    Require(clamped.shapeScale == 500.0f && clamped.detailScale == 10000.0f && clamped.weatherScale == 1000.0f, "scales clamped");
    Require(clamped.billows == 0.0f && clamped.forwardAnisotropy == 0.95f && clamped.backAnisotropy == 0.0f, "shape and lobes clamped");
    Require(clamped.backWeight == 1.0f && clamped.albedo == 1.0f && clamped.ambientScale == 0.0f && clamped.hazeDistance == 1000.0f,
            "weights clamped");
    Require(clamped.diffusion == 1.0f && clamped.ambientOcclusion == 0.0f, "diffusion and ambient occlusion clamped");
    Require(ClampCloudSettings(CloudSettings{}) == CloudSettings{}, "the defaults are inside the ranges");
}

void PlumeMap()
{
    // Away from every plume the top sits on the floor; the map wraps with the tile.
    int floorTexels = 0;
    int plumeTexels = 0;
    float highest = kCloudWeatherFloor;
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            const glm::vec2 uv((static_cast<float>(x) + 0.5f) / 64.0f, (static_cast<float>(y) + 0.5f) / 64.0f);
            const glm::vec2 texel = CloudWeatherTexel(uv);
            Require(texel.x >= kCloudWeatherFloor && texel.x <= 1.0f && texel.y >= 0.0f, "tops within [floor, 1], slopes positive");
            floorTexels += texel.x == kCloudWeatherFloor ? 1 : 0;
            plumeTexels += texel.x > 0.0f ? 1 : 0;
            highest = std::max(highest, texel.x);
            const glm::vec2 wrapped = CloudWeatherTexel(uv + glm::vec2(1.0f, -1.0f));
            Require(Near(wrapped.x, texel.x, 1e-4f), "the map wraps with the tile");
        }
    }
    Require(floorTexels > 0 && plumeTexels > 0, "clear air and plumes both");
    Require(highest > 0.4f && highest <= 1.0f, "the tallest plumes reach well up the layer");
}

void CoverageOffsetMatchesTheMap()
{
    // The share of a 256^2 sampling of the map whose tops clear the lowered base, against the
    // coverage asked for.
    constexpr int kSamples = 256;
    std::vector<float> tops;
    tops.reserve(kSamples * kSamples);
    for (int y = 0; y < kSamples; ++y)
    {
        for (int x = 0; x < kSamples; ++x)
        {
            tops.push_back(CloudWeatherTexel(glm::vec2((static_cast<float>(x) + 0.5f) / kSamples, (static_cast<float>(y) + 0.5f) / kSamples)).x);
        }
    }
    for (float coverage : {0.05f, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.75f, 0.9f})
    {
        const float offset = CloudCoverageOffset(coverage);
        size_t covered = 0;
        for (float top : tops)
        {
            covered += top > offset ? 1u : 0u;
        }
        const float share = static_cast<float>(covered) / static_cast<float>(tops.size());
        Require(Near(share, coverage, 0.04f), "coverage " + std::to_string(coverage) + " covers " + std::to_string(share));
    }
    Require(CloudCoverageOffset(0.0f) > 0.65f, "coverage 0 lowers every top below the base");
    Require(CloudCoverageOffset(1.0f) < kCloudWeatherFloor, "coverage 1 closes the layer");
    float previous = CloudCoverageOffset(0.0f);
    for (float coverage = 0.05f; coverage <= 1.0f; coverage += 0.05f)
    {
        Require(CloudCoverageOffset(coverage) < previous, "more coverage, lower offset");
        previous = CloudCoverageOffset(coverage);
    }
}

void SurfaceDistance()
{
    // A flat top 1 km up (0.4 of 2.5 km), no slope.
    Require(Near(CloudSurfaceDistance(0.4f, 0.0f, 0.0f, 2.5f, 0.025f, 0.6f), 0.4f, 1e-6f), "below a flat top, the distance up to it");
    Require(Near(CloudSurfaceDistance(0.4f, 0.0f, 0.0f, 2.5f, 0.025f, 0.1f), 0.1f, 1e-6f), "near the base, the distance down to it");
    Require(CloudSurfaceDistance(0.4f, 0.0f, 0.0f, 2.5f, 0.025f, 1.2f) < 0.0f, "above the top, outside");
    Require(CloudSurfaceDistance(0.4f, 0.0f, 0.0f, 2.5f, 0.025f, -0.1f) < 0.0f, "below the base, outside");
    Require(Near(CloudSurfaceDistance(0.4f, 0.0f, 0.2f, 2.5f, 0.025f, 0.6f), -0.1f, 1e-6f), "the coverage offset lowers the top");
    // A slope of 1 (45 degrees) measures across it: the vertical gap over sqrt 2.
    const float slopeUv = 1.0f / (2.5f * 0.025f);
    Require(Near(CloudSurfaceDistance(0.4f, slopeUv, 0.0f, 2.5f, 0.025f, 0.6f), 0.4f / std::sqrt(2.0f), 1e-5f), "a sloping side is measured across");
}

void BillowsAndProfile()
{
    const glm::vec4 mean(kCloudBillowMean);
    Require(CloudBillows(mean, mean, 7.0f, 0.6f, 1.0f, 1.0f, 1.0f) == 0.0f, "billows at their mean move nothing");
    Require(CloudBillows(glm::vec4(1.0f), glm::vec4(1.0f), 7.0f, 0.6f, 1.0f, 1.0f, 1.0f) > 0.0f, "high billows push the surface out");
    Require(CloudBillows(glm::vec4(0.0f), glm::vec4(0.0f), 7.0f, 0.6f, 1.0f, 1.0f, 1.0f) < 0.0f, "low ones cut it in");
    Require(CloudBillows(glm::vec4(1.0f), mean, 7.0f, 0.6f, 1.0f, 1.0f, 0.0f) == 0.0f, "no large billows at the base: it stays level");
    const float ragged = CloudBillows(mean, glm::vec4(1.0f), 7.0f, 0.6f, 1.0f, 1.0f, 0.0f);
    Require(Near(ragged, kCloudBaseRaggedness * CloudBillows(mean, glm::vec4(1.0f), 7.0f, 0.6f, 1.0f, 1.0f, 1.0f), 1e-6f),
            "the small ones leave it ragged");
    Require(CloudBillows(glm::vec4(1.0f), glm::vec4(1.0f), 7.0f, 0.6f, 0.0f, 1.0f, 1.0f) == 0.0f, "strength 0, smooth domes");
    const float noDetail = CloudBillows(glm::vec4(1.0f), glm::vec4(1.0f), 7.0f, 0.6f, 1.0f, 0.0f, 1.0f);
    const float withDetail = CloudBillows(glm::vec4(1.0f), glm::vec4(1.0f), 7.0f, 0.6f, 1.0f, 1.0f, 1.0f);
    Require(withDetail > noDetail && noDetail > 0.0f, "the detail octaves add the small billows");
    // The largest lobe is a fifth of its 1.75 km spacing at most above the mean.
    Require(Near(CloudBillows(glm::vec4(1.0f, kCloudBillowMean, kCloudBillowMean, 0.0f), mean, 7.0f, 0.6f, 1.0f, 0.0f, 1.0f),
                 (1.0f - kCloudBillowMean) * 0.05f * 7.0f, 1e-5f),
            "billow heights scale with the tile");

    Require(CloudWaterProfile(0.0f) == kCloudWaterAtBase, "thin water at the base");
    Require(CloudWaterProfile(kCloudWaterFullHeightKm) == 1.0f && CloudWaterProfile(3.0f) == 1.0f, "full from a kilometre up");
    Require(CloudWaterProfile(0.5f) > CloudWaterProfile(0.2f), "rising with height");

    Require(CloudEdgeDensity(-0.01f) == 0.0f && CloudEdgeDensity(0.0f) == 0.0f, "nothing outside the surface");
    Require(Near(CloudEdgeDensity(kCloudEdgeKm * 0.5f), 0.5f, 1e-6f) && CloudEdgeDensity(1.0f) == 1.0f, "full 15 m in");
}

void PhaseIsNormalized()
{
    for (float backWeight : {0.0f, 0.3f, 1.0f})
    {
        constexpr int kSteps = 20000;
        double integral = 0.0;
        for (int step = 0; step < kSteps; ++step)
        {
            const double cosTheta = -1.0 + (step + 0.5) * 2.0 / kSteps;
            integral += CloudPhase(0.8f, -0.3f, backWeight, static_cast<float>(cosTheta)) * 2.0 * kPi * (2.0 / kSteps);
        }
        Require(std::abs(integral - 1.0) < 2e-3, "the dual lobe integrates to 1 at back weight " + std::to_string(backWeight));
    }
    Require(CloudPhase(0.8f, -0.3f, 0.3f, 1.0f) > CloudPhase(0.8f, -0.3f, 0.3f, -1.0f), "silver lining: brightest toward the sun");
}

void SunScatteringOctaves()
{
    const float single = CloudPhase(0.8f, -0.3f, 0.3f, 0.2f);
    const float clear = CloudSunScattering(0.0f, 0.8f, -0.3f, 0.3f, 0.2f);
    Require(clear > single, "the octaves add light on top of single scattering");
    float octaveSum = 0.0f;
    for (int octave = 0; octave < kCloudScatteringOctaves; ++octave)
    {
        octaveSum += std::pow(kCloudOctaveScattering, static_cast<float>(octave));
    }
    Require(Near(CloudSunScattering(0.0f, 0.0f, 0.0f, 0.0f, 0.2f), octaveSum / static_cast<float>(4.0 * kPi), 1e-6f),
            "isotropic octaves sum to the geometric series of a");
    Require(kCloudOctaveScattering <= kCloudOctaveExtinction, "a <= b keeps the octaves energy conserving");
    float previous = clear;
    for (float depth : {0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 40.0f})
    {
        const float scattering = CloudSunScattering(depth, 0.8f, -0.3f, 0.3f, 0.2f);
        Require(scattering < previous, "deeper inside, less sun");
        previous = scattering;
    }
    // The last octaves see a fraction of the depth, so the core keeps light that single scattering
    // alone would lose: the grey, not black, underside.
    Require(CloudSunScattering(10.0f, 0.8f, -0.3f, 0.3f, 0.2f) > 100.0f * single * std::exp(-10.0f), "multiple scattering lights the core");
}

void DiffusionField()
{
    const float meanCosine = CloudMeanCosine(0.8f, -0.3f, 0.3f);
    Require(Near(meanCosine, 0.47f, 1e-6f), "the default lobes have a mean cosine of 0.47");
    Require(CloudMeanCosine(0.0f, -0.9f, 1.0f) == 0.0f && CloudMeanCosine(0.95f, 0.0f, 0.0f) == 0.95f, "the mean cosine is held to [0, 0.95]");

    // Lossless cloud: no decay, and the similarity scale is 1 - g.
    const glm::vec2 lossless = CloudDiffusionParameters(1.0f, 0.5f);
    Require(lossless.x == 0.0f && Near(lossless.y, 0.5f, 1e-6f), "a lossless cloud's diffusion field does not decay");
    const glm::vec2 cloud = CloudDiffusionParameters(0.98f, meanCosine);
    Require(Near(cloud.x, 0.3335f, 1e-3f), "albedo 0.98 decays by 0.33 per scaled optical depth");
    Require(CloudDiffusionParameters(0.2f, 0.0f).x == kCloudMaxDiffusionDecay, "the decay stays below 1");

    // A lossless half-space lit along the ray: the fluence is 2 E at the surface (Marshak) and
    // fills toward 5 E deep inside.
    const float perSteradian = static_cast<float>(1.0 / (4.0 * kPi));
    constexpr float kDeep = 1e6f;
    Require(Near(CloudDiffuseScattering(0.0f, kDeep, 0.0f, 1.0f), 2.0f * perSteradian, 1e-5f), "the surface fluence is 2 E");
    Require(Near(CloudDiffuseScattering(40.0f, kDeep, 0.0f, 1.0f), 5.0f * perSteradian, 1e-4f), "the lossless core fills to 5 E");

    // A finite cloud lets the light out the far side: the fluence falls linearly toward the face
    // the light leaves by, to 10 / (3 (T + 4/3)) E there.
    Require(Near(CloudDiffuseScattering(40.0f, 0.0f, 0.0f, 1.0f), 10.0f / (3.0f * (40.0f + 4.0f / 3.0f)) * perSteradian, 1e-5f),
            "at the far face of a tau 40 slab, a twelfth of the incident light");
    Require(CloudDiffuseScattering(0.0f, 0.0f, 0.0f, 1.0f) == 0.0f, "no cloud, no diffused light");
    float previousSlab = 10.0f;
    for (float depth : {5.0f, 10.0f, 20.0f, 30.0f, 39.0f})
    {
        const float slab = CloudDiffuseScattering(depth, 40.0f - depth, 0.0f, 1.0f);
        Require(slab < previousSlab, "falling toward the far face");
        Require(slab < CloudDiffuseScattering(depth, kDeep, 0.0f, 1.0f), "and below the half-space's");
        previousSlab = slab;
    }
    Require(CloudDiffuseScattering(1.0f, 1.0f, 0.0f, 1.0f) < CloudDiffuseScattering(1.0f, 20.0f, 0.0f, 1.0f), "thin clouds hold less");

    // Where both hold, at the lit surface, single scattering plus diffusion stays within a factor
    // of two of the octaves: two approximations of the same light.
    for (float cosTheta : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
    {
        const float octaves = CloudSunScattering(0.0f, 0.8f, -0.3f, 0.3f, cosTheta);
        const float diffused = CloudPhase(0.8f, -0.3f, 0.3f, cosTheta) + CloudDiffuseScattering(0.0f, kDeep, cloud.x, cloud.y);
        Require(diffused > 0.5f * octaves && diffused < 2.0f * octaves, "the models agree at the lit surface");
    }

    for (float depth : {0.0f, 0.5f, 1.0f, 3.0f, 10.0f, 20.0f, 60.0f})
    {
        const float octaves = CloudSunScattering(depth, 0.8f, -0.3f, 0.3f, 0.2f);
        Require(CloudSunScatteringWithDiffusion(depth, depth, kDeep, 0.0f, 0.0f, 0.0f, 0.8f, -0.3f, 0.3f, 0.2f, 0.0f, cloud.x, cloud.y) == octaves,
                "diffusion 0 leaves the octaves alone");
        const float half = CloudSunScatteringWithDiffusion(depth, depth, kDeep, 0.0f, 0.0f, 0.0f, 0.8f, -0.3f, 0.3f, 0.2f, 0.5f, cloud.x, cloud.y);
        const float full = CloudSunScatteringWithDiffusion(depth, depth, kDeep, 0.0f, 0.0f, 0.0f, 0.8f, -0.3f, 0.3f, 0.2f, 1.0f, cloud.x, cloud.y);
        Require(half >= octaves && full >= half, "diffusion only ever adds what the octaves miss");
    }
    // Ten optical depths in, the octaves are nearly gone; the diffusion field is not.
    Require(CloudSunScatteringWithDiffusion(10.0f, 10.0f, kDeep, 0.0f, 0.0f, 0.0f, 0.8f, -0.3f, 0.3f, 0.2f, 1.0f, cloud.x, cloud.y) >
                20.0f * CloudSunScattering(10.0f, 0.8f, -0.3f, 0.3f, 0.2f),
            "diffusion lights the core of a thick cloud");
}

void ColumnAndDeck()
{
    // The water column is the profile's integral: check it against a fine midpoint sum.
    for (float height : {0.05f, 0.125f, 0.4f, 1.0f, 1.7f})
    {
        constexpr int kSteps = 20000;
        double sum = 0.0;
        for (int step = 0; step < kSteps; ++step)
        {
            sum += CloudWaterProfile((static_cast<float>(step) + 0.5f) * height / kSteps) * height / kSteps;
        }
        Require(Near(CloudWaterColumn(height), static_cast<float>(sum), 1e-4f), "the column integrates the water profile at " + std::to_string(height));
    }
    Require(CloudWaterColumn(-1.0f) == 0.0f && CloudWaterColumn(0.0f) == 0.0f, "nothing below the base");
    Require(Near(CloudPlumeColumnDepth(0.6f, 20.0f), 20.0f * CloudWaterColumn(0.6f), 1e-6f), "depth is the column times the extinction");

    Require(CloudDeckWeight(0.0f) == 0.0f && CloudDeckWeight(0.45f) == 0.0f && CloudDeckWeight(kCloudDeckCoverageStart) == 0.0f,
            "separate cumulus keep the slanted path");
    Require(CloudDeckWeight(1.0f) == 1.0f, "a closed deck takes the column");
    float previous = 0.0f;
    for (float coverage = 0.6f; coverage <= 1.0f; coverage += 0.05f)
    {
        Require(CloudDeckWeight(coverage) >= previous, "rising with coverage");
        previous = CloudDeckWeight(coverage);
    }

    // Scattered light takes the column when that is shorter, only as far as the deck weight goes,
    // and never the column of a point the sun does not reach from above.
    Require(CloudScatteredOpticalDepth(40.0f, 6.4f, 0.64f, 1.0f) == 10.0f, "down the column in a deck");
    Require(CloudScatteredOpticalDepth(40.0f, 6.4f, 0.64f, 0.0f) == 40.0f, "the slanted path between cumulus");
    Require(Near(CloudScatteredOpticalDepth(40.0f, 6.4f, 0.64f, 0.5f), 25.0f, 1e-5f), "between, in between");
    Require(CloudScatteredOpticalDepth(2.0f, 6.4f, 0.64f, 1.0f) == 2.0f, "a shorter slanted path stands");
    Require(CloudScatteredOpticalDepth(40.0f, 0.0f, -0.2f, 1.0f) == 40.0f, "the sun below the horizon: the slanted path");
    Require(CloudScatteredOpticalDepth(40.0f, 1.0f, 0.01f, 1.0f) == 1.0f / kCloudMinSunCosine, "a low sun's column is measured at the floor");

    // A tower's hard shadow stays in single scattering: with the scattered light clear, the slanted
    // path still takes the first octave.
    const float clear = CloudSunScattering(0.0f, 0.8f, -0.3f, 0.3f, 0.2f);
    Require(Near(CloudSunScattering(1e3f, 0.0f, 0.8f, -0.3f, 0.3f, 0.2f), clear - CloudPhase(0.8f, -0.3f, 0.3f, 0.2f), 1e-6f),
            "the shadow takes single scattering only");
    Require(CloudSunScattering(3.0f, 3.0f, 0.8f, -0.3f, 0.3f, 0.2f) == CloudSunScattering(3.0f, 0.8f, -0.3f, 0.3f, 0.2f),
            "one depth for all, the plain octaves");

    // The fluence across a slab lit at an angle: mu = 1 is the slab along the ray.
    for (float scaled : {0.0f, 0.5f, 3.0f, 12.0f})
    {
        const float total = scaled + 7.0f;
        const float alongRay = 5.0f - 3.0f * std::exp(-scaled) - (5.0f - std::exp(-total)) * (scaled + 2.0f / 3.0f) / (total + 4.0f / 3.0f);
        Require(Near(CloudDiffuseFluence(scaled, total, 1.0f), alongRay, 1e-5f), "mu 1 is the field along the ray");
    }
    for (float mu : {0.2f, 0.5f, 0.8f})
    {
        Require(Near(CloudDiffuseFluence(0.0f, 1e7f, mu), 2.0f * mu, 1e-4f), "2 mu E at the lit face of a thick slab");
        Require(Near(CloudDiffuseFluence(40.0f, 1e7f, mu), mu * (3.0f * mu + 2.0f), 1e-3f), "mu (3 mu + 2) E deep inside");
        Require(CloudDiffuseFluence(0.0f, 0.0f, mu) == 0.0f, "no slab, no field");
        Require(CloudDiffuseFluence(2.0f, 10.0f, mu) < CloudDiffuseFluence(2.0f, 10.0f, 1.0f), "a slanted sun lights it less");
    }

    const glm::vec2 cloud = CloudDiffusionParameters(0.98f, CloudMeanCosine(0.8f, -0.3f, 0.3f));
    Require(CloudSlabDiffuseScattering(5.0f, 5.0f, 0.0f, cloud.x, cloud.y) == 0.0f &&
                CloudSlabDiffuseScattering(5.0f, 5.0f, -0.5f, cloud.x, cloud.y) == 0.0f,
            "no sun above the column, no field across it");
    Require(Near(CloudSlabDiffuseScattering(4.0f, 9.0f, 1.0f, cloud.x, cloud.y), CloudDiffuseScattering(4.0f, 9.0f, cloud.x, cloud.y), 1e-6f),
            "a sun overhead: the column is the ray");

    // The base of a 600 m deck beside a tower: the slanted path crosses 40 of cloud, the column 6.
    const float up = CloudPlumeColumnDepth(0.6f, 20.0f);
    const float slanted = CloudSunScatteringWithDiffusion(40.0f, 40.0f, 0.0f, up, 0.0f, 0.64f, 0.8f, -0.3f, 0.3f, -0.6f, 1.0f, cloud.x, cloud.y);
    // With diffusion off, the octaves alone: down the column they still reach it.
    const float deckOctaves = CloudSunScatteringWithDiffusion(
        40.0f, CloudScatteredOpticalDepth(40.0f, up, 0.64f, 1.0f), 0.0f, up, 0.0f, 0.64f, 0.8f, -0.3f, 0.3f, -0.6f, 0.0f, cloud.x, cloud.y);
    const float slantedOctaves = CloudSunScatteringWithDiffusion(40.0f, 40.0f, 0.0f, up, 0.0f, 0.64f, 0.8f, -0.3f, 0.3f, -0.6f, 0.0f, cloud.x, cloud.y);
    const float alongRay = CloudSunScatteringWithDiffusion(40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, -0.3f, 0.3f, -0.6f, 1.0f, cloud.x, cloud.y);
    Require(slanted > 10.0f * alongRay, "the column's diffusion field lights a deck's base the slanted ray left dark");
    Require(deckOctaves > 100.0f * slantedOctaves, "and the scattered octaves come down the column too");
}

void DiffuseTransmittance()
{
    Require(CloudDiffuseTransmittance(0.0f, 0.85f) == 1.0f, "no cloud, no occlusion");
    Require(Near(CloudDiffuseTransmittance(20.0f, 0.85f), 1.0f / 3.25f, 1e-6f), "a tau 20 water cloud lets about 30 % through");
    float previous = 1.0f;
    for (float depth : {0.5f, 2.0f, 10.0f, 50.0f})
    {
        const float transmittance = CloudDiffuseTransmittance(depth, 0.47f);
        Require(transmittance < previous && transmittance > std::exp(-depth), "thicker hides more, but far less than Beer");
        previous = transmittance;
    }
}

void ShellFromTheGround()
{
    const glm::vec3 camera(0.0f, kPlanet + 0.002f, 0.0f);
    const glm::vec2 up = CloudShellInterval(camera, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(Near(up.x, 1.498f, 1e-3f) && Near(up.y, 3.998f, 1e-3f), "straight up crosses the layer from its base to its top");

    const glm::vec2 down = CloudShellInterval(camera, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(down.y <= down.x, "looking down meets the ground first");

    const glm::vec2 level = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(level.y > level.x && level.x > 100.0f, "a level ray reaches the layer beyond the horizon");
    Require(Near(level.x, std::sqrt(kInner * kInner - camera.y * camera.y), 0.05f), "where the level ray rises through the base");

    const glm::vec2 clipped = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, 150.0f);
    Require(clipped.x == level.x && clipped.y == 150.0f, "the march stops at the maximum distance");
    const glm::vec2 beyond = CloudShellInterval(camera, glm::vec3(1.0f, 0.0f, 0.0f), kPlanet, kInner, kOuter, 50.0f);
    Require(beyond.y <= beyond.x, "nothing when the layer starts beyond the maximum distance");
}

void ShellFromInsideAndAbove()
{
    const glm::vec3 inside(0.0f, kInner + 1.0f, 0.0f);
    const glm::vec2 insideUp = CloudShellInterval(inside, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(insideUp.x == 0.0f && Near(insideUp.y, 1.5f, 1e-3f), "inside, the march starts at the camera");
    const glm::vec2 insideDown = CloudShellInterval(inside, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(insideDown.x == 0.0f && Near(insideDown.y, 1.0f, 1e-3f), "inside looking down, it ends at the base");

    const glm::vec3 above(0.0f, kOuter + 2.0f, 0.0f);
    const glm::vec2 aboveDown = CloudShellInterval(above, glm::vec3(0.0f, -1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(Near(aboveDown.x, 2.0f, 1e-3f) && Near(aboveDown.y, 4.5f, 1e-3f), "above, the layer runs from its top to its base");
    const glm::vec2 aboveUp = CloudShellInterval(above, glm::vec3(0.0f, 1.0f, 0.0f), kPlanet, kInner, kOuter, kMaxDistance);
    Require(aboveUp.y <= aboveUp.x, "above looking up sees no layer");
}
}

void ShadowMapProjection()
{
    const float texel = kCloudShadowExtentMeters / static_cast<float>(kCloudShadowMapSize);
    const glm::vec3 camera(100.0f, 30.0f, -40.0f);
    const glm::vec2 center = CloudShadowMapCenter(camera);
    Require(Near(center.x, std::floor(100.0f / texel) * texel, 1e-3f) && Near(center.y, std::floor(-40.0f / texel) * texel, 1e-3f),
            "the map follows the camera in whole texels");
    Require(CloudShadowMapCenter(camera + glm::vec3(0.0f, 500.0f, 0.0f)) == center, "height never moves the map");

    const glm::vec3 overhead(0.0f, 1.0f, 0.0f);
    const glm::vec2 ground = CloudShadowUv(glm::vec3(center.x, 0.0f, center.y), overhead, camera);
    Require(Near(ground.x, 0.5f, 1e-6f) && Near(ground.y, 0.5f, 1e-6f), "the ground under the centre is the map's centre");
    const glm::vec2 roof = CloudShadowUv(glm::vec3(center.x, 50.0f, center.y), overhead, camera);
    Require(roof == ground, "with the sun overhead, height does not move the lookup");

    // The sun 45 degrees up toward +x: a point 100 m up looks at the ground 100 m toward -x of the
    // point below it, where the same ray to the sun starts.
    const glm::vec3 slanted = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec2 raised = CloudShadowUv(glm::vec3(center.x, 100.0f, center.y), slanted, camera);
    Require(Near(raised.x, 0.5f - 100.0f / kCloudShadowExtentMeters, 1e-5f) && Near(raised.y, 0.5f, 1e-6f),
            "a raised point looks up the map along the sun's ray");

    const glm::vec2 setting = CloudShadowUv(glm::vec3(center.x, 10.0f, center.y), glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f)), camera);
    Require(std::isfinite(setting.x) && Near(setting.x, 0.5f - 10.0f / kCloudShadowMinSunHeight / kCloudShadowExtentMeters, 1e-4f),
            "a sun at the horizon is held at the minimum height");

    Require(CloudShadowEdgeWeight(glm::vec2(0.5f)) == 1.0f, "full shadow inside");
    Require(CloudShadowEdgeWeight(glm::vec2(0.0f, 0.5f)) == 0.0f && CloudShadowEdgeWeight(glm::vec2(1.2f, 0.5f)) == 0.0f, "none at and past the edge");
    Require(Near(CloudShadowEdgeWeight(glm::vec2(0.5f, kCloudShadowEdgeFade * 0.5f)), 0.5f, 1e-5f), "fading over the outer tenth");
}

int main()
{
    try
    {
        ClampsSettings();
        PlumeMap();
        CoverageOffsetMatchesTheMap();
        SurfaceDistance();
        BillowsAndProfile();
        PhaseIsNormalized();
        SunScatteringOctaves();
        DiffusionField();
        ColumnAndDeck();
        DiffuseTransmittance();
        ShellFromTheGround();
        ShellFromInsideAndAbove();
        ShadowMapProjection();
    }
    catch (const std::exception& error)
    {
        std::cerr << "volumetric clouds tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "volumetric clouds tests passed\n";
    return 0;
}
