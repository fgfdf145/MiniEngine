#include "volumetric_clouds.h"

#include "height_fog.h"

#include <engine/scene/wind.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;

float Saturate(float x)
{
    return std::clamp(x, 0.0f, 1.0f);
}

// Both roots of |origin + t direction| = radius; false when the line misses the sphere.
bool SphereRoots(const glm::vec3& origin, const glm::vec3& direction, float radius, float& nearT, float& farT)
{
    const float b = glm::dot(origin, direction);
    const float c = glm::dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return false;
    }
    const float root = std::sqrt(discriminant);
    nearT = -b - root;
    farT = -b + root;
    return true;
}

// The weather map's generator, as cloud_weather.comp: Jarzynski and Olano's pcg3d.
constexpr uint32_t kWeatherSeed = 211u;
constexpr uint32_t kWeatherHeightSeed = 223u;
constexpr uint32_t kClusterSeed = 101u;

glm::uvec3 Pcg3d(glm::uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

glm::vec3 WeatherRandom(const glm::ivec2& cell, uint32_t slot, uint32_t seed)
{
    const glm::uvec3 v =
        Pcg3d(glm::uvec3(static_cast<uint32_t>(cell.x), static_cast<uint32_t>(cell.y), slot) + glm::uvec3(seed, seed * 7u, seed * 13u));
    return glm::vec3(v & 0xffffu) / 65535.0f;
}

// Smooth value noise over period cells of the tile, wrapping.
float WeatherValueNoise(const glm::vec2& uv, int period, uint32_t seed)
{
    const glm::vec2 grid = uv * static_cast<float>(period);
    const glm::ivec2 cell = glm::ivec2(glm::floor(grid));
    glm::vec2 f = grid - glm::vec2(cell);
    f = f * f * (3.0f - 2.0f * f);
    const auto at = [&](int x, int y)
    {
        const glm::ivec2 wrapped = ((cell + glm::ivec2(x, y)) % period + period) % period;
        return WeatherRandom(wrapped, 0u, seed).x;
    };
    return glm::mix(glm::mix(at(0, 0), at(1, 0), f.x), glm::mix(at(0, 1), at(1, 1), f.x), f.y);
}

// Where the plumes gather: two octaves of smooth noise, 10 and 5 km across on the 40 km tile.
float WeatherCluster(const glm::vec2& uv)
{
    return 0.6f * WeatherValueNoise(uv, 4, kClusterSeed) + 0.4f * WeatherValueNoise(uv, 8, kClusterSeed + 2u);
}

// One dome of radius and height (grid units, share of the thickness) at centre, sunk by the share
// 1 - life of its height, joined to the top so far by the smooth maximum; its slope per uv follows.
void WeatherDome(const glm::vec2& grid, const glm::vec2& centre, float radius, float height, float life, float cells, float& top, float& slope)
{
    const float d = glm::length(grid - centre) / radius;
    const float dome = height * (life - std::pow(d, kCloudDomeExponent));
    const float domeSlope = height * kCloudDomeExponent * std::pow(d, kCloudDomeExponent - 1.0f) / radius * cells;
    // Polynomial smooth maximum; the slope follows the blend.
    const float blend = std::clamp(0.5f + 0.5f * (dome - top) / kCloudDomeBlend, 0.0f, 1.0f);
    top = glm::mix(top, dome, blend) + kCloudDomeBlend * blend * (1.0f - blend);
    slope = glm::mix(slope, domeSlope, blend);
}

// CloudCoverageOffset's knots: (share of the ground under a plume, how far the tops are lowered),
// measured over the 512^2 map at four sets of life phases pooled (tests/volumetric_clouds_tests.cpp,
// CoverageOffsetMatchesTheMap): the plumes at every stage of their lives cover the same share of
// the ground whenever it is measured, within a share of 0.03 at 5 % coverage and far less above.
// Below the floor the whole layer closes.
constexpr std::array<glm::vec2, 12> kCoverageKnots = {
    glm::vec2(0.00f, 0.7000f),
    glm::vec2(0.05f, 0.1898f),
    glm::vec2(0.10f, 0.1018f),
    glm::vec2(0.20f, 0.0408f),
    glm::vec2(0.30f, 0.0169f),
    glm::vec2(0.40f, -0.0011f),
    glm::vec2(0.50f, -0.0190f),
    glm::vec2(0.60f, -0.0405f),
    glm::vec2(0.70f, -0.0680f),
    glm::vec2(0.80f, -0.1068f),
    glm::vec2(0.90f, -0.1751f),
    glm::vec2(1.00f, -0.3000f),
};
}

CloudSettings ClampCloudSettings(const CloudSettings& settings)
{
    CloudSettings clamped = settings;
    clamped.coverage = std::clamp(settings.coverage, 0.0f, 1.0f);
    clamped.baseAltitude = std::clamp(settings.baseAltitude, 100.0f, 10000.0f);
    clamped.thickness = std::clamp(settings.thickness, 100.0f, 10000.0f);
    clamped.density = std::clamp(settings.density, 0.001f, 0.5f);
    clamped.shapeScale = std::clamp(settings.shapeScale, 500.0f, 100000.0f);
    clamped.detailScale = std::clamp(settings.detailScale, 50.0f, 10000.0f);
    clamped.weatherScale = std::clamp(settings.weatherScale, 1000.0f, 500000.0f);
    clamped.billows = std::clamp(settings.billows, 0.0f, 2.0f);
    clamped.forwardAnisotropy = std::clamp(settings.forwardAnisotropy, 0.0f, 0.95f);
    clamped.backAnisotropy = std::clamp(settings.backAnisotropy, -0.95f, 0.0f);
    clamped.backWeight = std::clamp(settings.backWeight, 0.0f, 1.0f);
    clamped.albedo = std::clamp(settings.albedo, 0.0f, 1.0f);
    clamped.ambientScale = std::clamp(settings.ambientScale, 0.0f, 4.0f);
    clamped.hazeDistance = std::clamp(settings.hazeDistance, 1000.0f, 1000000.0f);
    clamped.diffusion = std::clamp(settings.diffusion, 0.0f, 1.0f);
    clamped.ambientOcclusion = std::clamp(settings.ambientOcclusion, 0.0f, 1.0f);
    clamped.updraft = std::clamp(settings.updraft, 0.0f, 10.0f);
    clamped.lifetime = std::clamp(settings.lifetime, 1.0f, 240.0f);
    clamped.timeScale = std::clamp(settings.timeScale, 0.0f, 3600.0f);
    return clamped;
}

glm::vec2 CloudWindVelocity(const SceneEnvironment& environment)
{
    const CloudSettings clouds = ClampCloudSettings(environment.clouds);
    const glm::vec3 velocity = WindVelocity(environment, clouds.baseAltitude + 0.5f * clouds.thickness);
    return glm::vec2(velocity.x, velocity.z);
}

CloudMotion AdvanceCloudMotion(const CloudMotion& motion, const SceneEnvironment& environment, float deltaSeconds)
{
    const CloudSettings clouds = ClampCloudSettings(environment.clouds);
    const double seconds =
        std::isfinite(deltaSeconds) ? static_cast<double>(std::clamp(deltaSeconds, 0.0f, 0.25f)) * clouds.timeScale : 0.0;
    CloudMotion next = motion;
    const glm::vec2 wind = CloudWindVelocity(environment);
    next.stepMeters = wind * static_cast<float>(seconds);
    next.windKm += glm::dvec2(wind) * (seconds * 0.001);
    next.riseKm += static_cast<double>(clouds.updraft) * seconds * 0.001;
    const double lifeSeconds = static_cast<double>(clouds.lifetime) * 60.0;
    for (size_t level = 0; level < kCloudPlumeLifeScale.size(); ++level)
    {
        const int index = static_cast<int>(level);
        // Whole lives fall away: only the phase matters, and it keeps its precision.
        const double lives = next.lives[index] + seconds / (lifeSeconds * kCloudPlumeLifeScale[level]);
        next.lives[index] = lives - std::floor(lives);
    }
    next.seconds += seconds;
    if (next.seconds - next.mapSeconds >= kCloudMapRefreshSeconds)
    {
        next.mapLives = next.lives;
        next.mapSeconds = next.seconds;
    }
    return next;
}

glm::vec4 CloudLifePhases(const CloudMotion& motion)
{
    return glm::vec4(glm::fract(motion.mapLives));
}

float CloudPlumeLife(float phase)
{
    const float p = glm::fract(phase);
    return glm::smoothstep(0.0f, kCloudLifeGrowth, p) * (1.0f - glm::smoothstep(kCloudLifeDecayStart, 1.0f, p));
}

float CloudRemap(float x, float a, float b, float c, float d)
{
    const float span = b - a;
    return span == 0.0f ? c : c + (x - a) / span * (d - c);
}

CloudPlumeCell CloudPlumeCellAt(size_t level, const glm::ivec2& cell)
{
    const CloudPlumeLevel& plumes = kCloudPlumeLevels[level];
    const uint32_t slot = static_cast<uint32_t>(level) * 8u;
    const glm::vec3 placement = WeatherRandom(cell, slot, kWeatherSeed);
    CloudPlumeCell plume;
    const float cluster = WeatherCluster(glm::fract((glm::vec2(cell) + glm::vec2(placement.y, placement.z)) / plumes.cells));
    if (placement.x > plumes.probability * (0.4f + 1.2f * cluster))
    {
        return plume;
    }
    const glm::vec3 shape = WeatherRandom(cell, slot + 1u, kWeatherSeed);
    const float radius = glm::mix(plumes.radiusMin, plumes.radiusMax, shape.x);
    const float height =
        std::min(glm::mix(plumes.aspectMin, plumes.aspectMax, shape.y) * 2.0f * radius / plumes.cells * kCloudPlumeHeightPerUv, 1.0f);
    plume.core = glm::vec4(placement.y, placement.z, plumes.turrets ? 0.7f * radius : radius, height);
    const int turrets = plumes.turrets ? 3 + static_cast<int>(shape.z * 2.999f) : 0;
    plume.life = glm::vec4(WeatherRandom(cell, slot + 7u, kWeatherHeightSeed).x, static_cast<float>(turrets), 0.0f, 0.0f);
    for (int turret = 0; turret < turrets; ++turret)
    {
        const uint32_t turretSlot = slot + 2u + static_cast<uint32_t>(turret);
        const glm::vec3 draw = WeatherRandom(cell, turretSlot, kWeatherSeed);
        const glm::vec3 turretDraw = WeatherRandom(cell, turretSlot, kWeatherHeightSeed);
        const float turretRadius = radius * glm::mix(0.4f, 0.65f, draw.x);
        const float angle = draw.y * 2.0f * kPi;
        const float offset = glm::mix(0.2f, 1.0f, draw.z) * (radius - turretRadius);
        plume.turrets[static_cast<size_t>(turret)] = glm::vec4(
            std::cos(angle) * offset, std::sin(angle) * offset, turretRadius, std::min(height * glm::mix(0.65f, 1.05f, turretDraw.x), 1.0f));
        plume.bobs[static_cast<size_t>(turret / 4)][turret % 4] = turretDraw.y;
    }
    return plume;
}

std::vector<CloudPlumeCell> BuildCloudPlumeTable()
{
    std::vector<CloudPlumeCell> table(kCloudPlumeTableSize);
    for (size_t level = 0; level < kCloudPlumeLevels.size(); ++level)
    {
        const int cells = static_cast<int>(kCloudPlumeLevels[level].cells);
        for (int y = 0; y < cells; ++y)
        {
            for (int x = 0; x < cells; ++x)
            {
                table[kCloudPlumeTableOffsets[level] + static_cast<uint32_t>(y * cells + x)] = CloudPlumeCellAt(level, glm::ivec2(x, y));
            }
        }
    }
    return table;
}

glm::vec2 CloudWeatherTexel(const glm::vec2& uv, const glm::vec4& lifePhases)
{
    float top = kCloudWeatherFloor;
    float slope = 0.0f;
    for (size_t level = 0; level < kCloudPlumeLevels.size(); ++level)
    {
        const CloudPlumeLevel& plumes = kCloudPlumeLevels[level];
        const int cells = static_cast<int>(plumes.cells);
        const glm::vec2 grid = uv * plumes.cells;
        const glm::ivec2 base = glm::ivec2(glm::floor(grid));
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const glm::ivec2 cell = base + glm::ivec2(dx, dy);
                const CloudPlumeCell plume = CloudPlumeCellAt(level, (cell % cells + cells) % cells);
                if (plume.core.w < 0.0f)
                {
                    continue;
                }
                const glm::vec2 centre = glm::vec2(cell) + glm::vec2(plume.core);
                const float phase = glm::fract(lifePhases[static_cast<int>(level)] + plume.life.x);
                const float life = CloudPlumeLife(phase);
                WeatherDome(grid, centre, plume.core.z, plume.core.w, life, plumes.cells, top, slope);
                const int turrets = static_cast<int>(plume.life.y);
                for (int turret = 0; turret < turrets; ++turret)
                {
                    const glm::vec4 shape = plume.turrets[static_cast<size_t>(turret)];
                    const float bob = 0.5f + 0.5f * std::cos(2.0f * kPi * glm::fract(2.0f * phase + plume.bobs[static_cast<size_t>(turret / 4)][turret % 4]));
                    WeatherDome(grid, centre + glm::vec2(shape), shape.z, shape.w, life * glm::mix(kCloudTurretLowest, 1.0f, bob), plumes.cells, top, slope);
                }
            }
        }
    }
    return glm::vec2(std::max(top, kCloudWeatherFloor), slope);
}

float CloudCoverageOffset(float coverage)
{
    // The share of the tile under a plume (top above the base) as the tops are lowered by each
    // knot, measured over CloudWeatherTexel (tests/volumetric_clouds_tests.cpp checks it).
    const float c = Saturate(coverage);
    for (size_t knot = 1; knot < kCoverageKnots.size(); ++knot)
    {
        if (c <= kCoverageKnots[knot].x)
        {
            const glm::vec2 a = kCoverageKnots[knot - 1];
            const glm::vec2 b = kCoverageKnots[knot];
            return glm::mix(a.y, b.y, (c - a.x) / (b.x - a.x));
        }
    }
    return kCoverageKnots.back().y;
}

float CloudSurfaceDistance(float top, float slope, float coverageOffset, float thicknessKm, float weatherFrequency, float heightKm)
{
    const float topKm = (top - coverageOffset) * thicknessKm;
    const float slopeKm = slope * thicknessKm * weatherFrequency;
    return std::min((topKm - heightKm) / std::sqrt(1.0f + slopeKm * slopeKm), heightKm);
}

float CloudBillows(const glm::vec4& shape, const glm::vec4& fine, float shapeTileKm, float detailTileKm, float strength, float detail, float heightKm)
{
    float large = 0.0f;
    for (size_t octave = 0; octave < kCloudShapeBillowPerTile.size(); ++octave)
    {
        large += (shape[static_cast<int>(octave)] - kCloudBillowMean) * kCloudShapeBillowPerTile[octave];
    }
    float small = 0.0f;
    for (size_t octave = 0; octave < kCloudDetailBillowPerTile.size(); ++octave)
    {
        small += (fine[static_cast<int>(octave)] - kCloudBillowMean) * kCloudDetailBillowPerTile[octave];
    }
    const float rise = Saturate(heightKm / kCloudBillowRiseKm);
    return strength * (rise * large * shapeTileKm + std::max(rise, kCloudBaseRaggedness) * detail * small * detailTileKm);
}

float CloudBaseReachKm(float shapeTileKm, float detailTileKm, float strength)
{
    float large = 0.0f;
    for (const float perTile : kCloudShapeBillowPerTile)
    {
        large += perTile;
    }
    float small = 0.0f;
    for (const float perTile : kCloudDetailBillowPerTile)
    {
        small += perTile;
    }
    return (1.0f - kCloudBillowMean) * strength * (kCloudBaseLargeBillows * large * shapeTileKm + kCloudBaseRaggedness * small * detailTileKm);
}

float CloudBaseDensity(const glm::vec4& shape, const glm::vec4& fine, float shapeTileKm, float detailTileKm, float strength, float detail, float heightKm)
{
    float large = 0.0f;
    for (size_t octave = 0; octave < kCloudShapeBillowPerTile.size(); ++octave)
    {
        large += (shape[static_cast<int>(octave)] - kCloudBillowMean) * kCloudShapeBillowPerTile[octave];
    }
    float small = 0.0f;
    for (size_t octave = 0; octave < kCloudDetailBillowPerTile.size(); ++octave)
    {
        small += (fine[static_cast<int>(octave)] - kCloudBillowMean) * kCloudDetailBillowPerTile[octave];
    }
    const float push = strength * (kCloudBaseLargeBillows * large * shapeTileKm + kCloudBaseRaggedness * detail * small * detailTileKm);
    const float baseKm = CloudBaseReachKm(shapeTileKm, detailTileKm, strength) - push;
    return Saturate((heightKm - baseKm) / kCloudBaseFadeKm);
}

float CloudWaterProfile(float heightKm)
{
    return std::max(kCloudWaterAtBase, std::pow(Saturate(heightKm / kCloudWaterFullHeightKm), 2.0f / 3.0f));
}

float CloudEdgeDensity(float distanceKm)
{
    return Saturate(distanceKm / kCloudEdgeKm);
}

float CloudWaterColumn(float heightKm)
{
    // The profile is flat at kCloudWaterAtBase up to where (h / H)^(2/3) reaches it, H a^(3/2),
    // rises as that power to H, whose integral is (3/5) H^(-2/3) h^(5/3), and is 1 above.
    const float h = std::max(heightKm, 0.0f);
    const float baseTop = kCloudWaterFullHeightKm * std::pow(kCloudWaterAtBase, 1.5f);
    if (h <= baseTop)
    {
        return kCloudWaterAtBase * h;
    }
    const float rising = 0.6f * std::pow(kCloudWaterFullHeightKm, -2.0f / 3.0f);
    const float full = std::min(h, kCloudWaterFullHeightKm);
    const float column = kCloudWaterAtBase * baseTop + rising * (std::pow(full, 5.0f / 3.0f) - std::pow(baseTop, 5.0f / 3.0f));
    return column + std::max(h - kCloudWaterFullHeightKm, 0.0f);
}

float CloudPlumeColumnDepth(float heightKm, float extinctionPerKm)
{
    return CloudWaterColumn(heightKm) * extinctionPerKm;
}

float CloudDeckWeight(float coverage)
{
    const float x = Saturate((coverage - kCloudDeckCoverageStart) / (1.0f - kCloudDeckCoverageStart));
    return x * x * (3.0f - 2.0f * x);
}

float CloudPhase(float forwardG, float backG, float backWeight, float cosTheta)
{
    return HenyeyGreenstein(forwardG, cosTheta) * (1.0f - backWeight) + HenyeyGreenstein(backG, cosTheta) * backWeight;
}

float CloudSunScattering(float lightOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta)
{
    return CloudSunScattering(lightOpticalDepth, lightOpticalDepth, forwardG, backG, backWeight, cosTheta);
}

float CloudSunScattering(float lightOpticalDepth, float scatteredOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta)
{
    float scattering = 0.0f;
    float a = 1.0f;
    float b = 1.0f;
    float c = 1.0f;
    for (int octave = 0; octave < kCloudScatteringOctaves; ++octave)
    {
        const float depth = octave == 0 ? lightOpticalDepth : scatteredOpticalDepth;
        scattering += a * CloudPhase(forwardG * c, backG * c, backWeight, cosTheta) * std::exp(-b * depth);
        a *= kCloudOctaveScattering;
        b *= kCloudOctaveExtinction;
        c *= kCloudOctaveAnisotropy;
    }
    return scattering;
}

float CloudScatteredOpticalDepth(float lightOpticalDepth, float upOpticalDepth, float sunCosine, float deckWeight)
{
    if (sunCosine <= 0.0f)
    {
        return lightOpticalDepth;
    }
    const float column = upOpticalDepth / std::max(sunCosine, kCloudMinSunCosine);
    return glm::mix(lightOpticalDepth, std::min(lightOpticalDepth, column), deckWeight);
}

float CloudMeanCosine(float forwardG, float backG, float backWeight)
{
    return std::clamp(forwardG * (1.0f - backWeight) + backG * backWeight, 0.0f, 0.95f);
}

glm::vec2 CloudDiffusionParameters(float albedo, float meanCosine)
{
    // Similarity with f = g: the forward peak counts as unscattered, the rest scatters
    // isotropically with albedo' = (1 - g) albedo / (1 - albedo g) over (1 - albedo g) tau.
    const float similarity = 1.0f - albedo * meanCosine;
    const float scaledAlbedo = similarity > 0.0f ? (1.0f - meanCosine) * albedo / similarity : 1.0f;
    const float kappa = std::min(std::sqrt(std::max(3.0f * (1.0f - scaledAlbedo), 0.0f)), kCloudMaxDiffusionDecay);
    return glm::vec2(kappa, similarity);
}

float CloudDiffuseScattering(float lightOpticalDepth, float awayOpticalDepth, float kappa, float similarity)
{
    // Lossless Eddington across a slab lit along the ray, Marshak at both faces: with D = 1 / 3,
    // phi'' = -3 E exp(-tau'), phi(0) = 2/3 phi'(0), phi(T) = -2/3 phi'(T) gives
    // phi = E (5 - 3 exp(-tau') - (5 - exp(-T')) (tau' + 2/3) / (T' + 4/3)): the half-space's
    // 5 - 3 exp(-tau') deep in a thick cloud, falling linearly to 10 / (3 (T' + 4/3)) at the face
    // the light leaves by. Absorption takes exp(-kappa tau') on top.
    const float scaled = similarity * lightOpticalDepth;
    const float total = similarity * (lightOpticalDepth + std::max(awayOpticalDepth, 0.0f));
    return CloudDiffuseFluence(scaled, total, 1.0f) * std::exp(-kappa * scaled) / (4.0f * kPi);
}

float CloudDiffuseFluence(float scaled, float total, float mu)
{
    // The same slab with the beam at cosine mu to its normal: phi'' = -3 E exp(-tau / mu) has the
    // particular part -3 mu^2 E exp(-tau / mu); with phi = A + B tau + that, the Marshak faces give
    // B = -(mu (3 mu + 2) + (2 mu - 3 mu^2) exp(-T / mu)) / (T + 4/3) and
    // phi = mu (3 mu + 2) - 3 mu^2 exp(-tau / mu) + B (tau + 2/3), mu = 1 the slab along the ray.
    const float deep = mu * (3.0f * mu + 2.0f);
    const float leaving = deep + (2.0f * mu - 3.0f * mu * mu) * std::exp(-total / mu);
    return std::max(deep - 3.0f * mu * mu * std::exp(-scaled / mu) - leaving * (scaled + 2.0f / 3.0f) / (total + 4.0f / 3.0f), 0.0f);
}

float CloudSlabDiffuseScattering(float upOpticalDepth, float downOpticalDepth, float sunCosine, float kappa, float similarity)
{
    if (sunCosine <= 0.0f)
    {
        return 0.0f;
    }
    const float scaled = similarity * std::max(upOpticalDepth, 0.0f);
    const float total = scaled + similarity * std::max(downOpticalDepth, 0.0f);
    return CloudDiffuseFluence(scaled, total, sunCosine) * std::exp(-kappa * scaled) / (4.0f * kPi);
}

float CloudSunScatteringWithDiffusion(
    float lightOpticalDepth,
    float scatteredOpticalDepth,
    float awayOpticalDepth,
    float upOpticalDepth,
    float downOpticalDepth,
    float sunCosine,
    float forwardG,
    float backG,
    float backWeight,
    float cosTheta,
    float diffusion,
    float kappa,
    float similarity)
{
    const float octaves = CloudSunScattering(lightOpticalDepth, scatteredOpticalDepth, forwardG, backG, backWeight, cosTheta);
    if (diffusion <= 0.0f)
    {
        return octaves;
    }
    const float single = CloudPhase(forwardG, backG, backWeight, cosTheta) * std::exp(-lightOpticalDepth);
    const float field = std::max(
        CloudDiffuseScattering(lightOpticalDepth, awayOpticalDepth, kappa, similarity),
        CloudSlabDiffuseScattering(upOpticalDepth, downOpticalDepth, sunCosine, kappa, similarity));
    const float diffused = single + field;
    return octaves + diffusion * std::max(diffused - octaves, 0.0f);
}

float CloudDiffuseTransmittance(float opticalDepth, float meanCosine)
{
    return 1.0f / (1.0f + 0.75f * (1.0f - meanCosine) * std::max(opticalDepth, 0.0f));
}

glm::vec2 CloudShadowMapCenter(const glm::vec3& cameraPosition)
{
    const float texel = kCloudShadowExtentMeters / static_cast<float>(kCloudShadowMapSize);
    return glm::floor(glm::vec2(cameraPosition.x, cameraPosition.z) / texel) * texel;
}

glm::vec2 CloudShadowUv(const glm::vec3& worldPosition, const glm::vec3& directionToSun, const glm::vec3& cameraPosition)
{
    const float height = std::max(directionToSun.y, kCloudShadowMinSunHeight);
    const glm::vec2 ground =
        glm::vec2(worldPosition.x, worldPosition.z) - glm::vec2(directionToSun.x, directionToSun.z) * (worldPosition.y / height);
    return (ground - CloudShadowMapCenter(cameraPosition)) / kCloudShadowExtentMeters + 0.5f;
}

float CloudShadowEdgeWeight(const glm::vec2& uv)
{
    const glm::vec2 edge = glm::min(uv, 1.0f - uv);
    return Saturate(std::min(edge.x, edge.y) / kCloudShadowEdgeFade);
}

glm::vec2 CloudShellInterval(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float planet,
    float inner,
    float outer,
    float maxDistance)
{
    constexpr glm::vec2 kNone(0.0f, -1.0f);
    const float radius = glm::length(origin);
    float outerNear = 0.0f;
    float outerFar = 0.0f;
    if (!SphereRoots(origin, direction, outer, outerNear, outerFar) || outerFar <= 0.0f)
    {
        return kNone;
    }
    float innerNear = 0.0f;
    float innerFar = 0.0f;
    const bool innerHit = SphereRoots(origin, direction, inner, innerNear, innerFar);

    float start = 0.0f;
    float end = outerFar;
    if (radius < inner)
    {
        // Below the layer: it starts where the ray leaves the inner sphere, unless the planet
        // stops it first.
        float planetNear = 0.0f;
        float planetFar = 0.0f;
        if (SphereRoots(origin, direction, planet, planetNear, planetFar) && planetNear > 0.0f)
        {
            return kNone;
        }
        start = innerFar;
    }
    else
    {
        // Inside or above: from here or the outer sphere down to the inner sphere, if it is met.
        start = std::max(outerNear, 0.0f);
        if (innerHit && innerNear > 0.0f)
        {
            end = std::min(end, innerNear);
        }
    }
    start = std::max(start, 0.0f);
    end = std::min(end, maxDistance);
    return end > start ? glm::vec2(start, end) : kNone;
}
}
