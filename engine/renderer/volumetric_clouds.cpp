#include "volumetric_clouds.h"

#include "height_fog.h"

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
    clamped.detailErosion = std::clamp(settings.detailErosion, 0.0f, 1.0f);
    clamped.forwardAnisotropy = std::clamp(settings.forwardAnisotropy, 0.0f, 0.95f);
    clamped.backAnisotropy = std::clamp(settings.backAnisotropy, -0.95f, 0.0f);
    clamped.backWeight = std::clamp(settings.backWeight, 0.0f, 1.0f);
    clamped.albedo = std::clamp(settings.albedo, 0.0f, 1.0f);
    clamped.ambientScale = std::clamp(settings.ambientScale, 0.0f, 4.0f);
    clamped.hazeDistance = std::clamp(settings.hazeDistance, 1000.0f, 1000000.0f);
    clamped.diffusion = std::clamp(settings.diffusion, 0.0f, 1.0f);
    clamped.ambientOcclusion = std::clamp(settings.ambientOcclusion, 0.0f, 1.0f);
    return clamped;
}

float CloudRemap(float x, float a, float b, float c, float d)
{
    const float span = b - a;
    return span == 0.0f ? c : c + (x - a) / span * (d - c);
}

float CloudHeightGradient(float heightFraction)
{
    const float base = Saturate(CloudRemap(heightFraction, 0.0f, 0.1f, 0.0f, 1.0f));
    const float top = Saturate(CloudRemap(heightFraction, 0.3f, 1.0f, 1.0f, 0.0f));
    return base * top;
}

float CloudWeather(float first, float second)
{
    return Saturate(CloudRemap(first * 0.65f + second * 0.35f, 0.25f, 0.75f, 0.0f, 1.0f));
}

float CloudShape(const glm::vec4& shape)
{
    const float fbm = shape.g * 0.625f + shape.b * 0.25f + shape.a * 0.125f;
    const float erosion = (1.0f - fbm) * 0.6f;
    return Saturate(CloudRemap(shape.r, erosion, 1.0f, 0.0f, 1.0f));
}

float CloudField(float weather, float shape, float gradient)
{
    return (weather * kCloudWeatherShare + shape * (1.0f - kCloudWeatherShare)) * gradient;
}

float CloudCoverageRamp(float field, float coverage)
{
    return coverage <= 0.0f ? 0.0f : Saturate((field - (1.0f - coverage)) / kCloudEdgeWidth);
}

float CloudPhase(float forwardG, float backG, float backWeight, float cosTheta)
{
    return HenyeyGreenstein(forwardG, cosTheta) * (1.0f - backWeight) + HenyeyGreenstein(backG, cosTheta) * backWeight;
}

float CloudSunScattering(float lightOpticalDepth, float forwardG, float backG, float backWeight, float cosTheta)
{
    float scattering = 0.0f;
    float a = 1.0f;
    float b = 1.0f;
    float c = 1.0f;
    for (int octave = 0; octave < kCloudScatteringOctaves; ++octave)
    {
        scattering += a * CloudPhase(forwardG * c, backG * c, backWeight, cosTheta) * std::exp(-b * lightOpticalDepth);
        a *= kCloudOctaveScattering;
        b *= kCloudOctaveExtinction;
        c *= kCloudOctaveAnisotropy;
    }
    return scattering;
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

float CloudDiffuseScattering(float lightOpticalDepth, float kappa, float similarity)
{
    // D phi'' - sigma_a phi = -sigma_s E exp(-tau'), D = 1 / 3, with phi(0) = 2 D phi'(0):
    // phi = 3 albedo' E / (1 - kappa^2) * (5/3 / (1 + 2 kappa / 3) exp(-kappa tau') - exp(-tau')),
    // and 3 albedo' = 3 - kappa^2.
    const float scaled = similarity * lightOpticalDepth;
    const float amplitude = (3.0f - kappa * kappa) / (1.0f - kappa * kappa);
    const float boundary = (5.0f / 3.0f) / (1.0f + 2.0f * kappa / 3.0f);
    const float fluence = amplitude * std::max(boundary * std::exp(-kappa * scaled) - std::exp(-scaled), 0.0f);
    return fluence / (4.0f * kPi);
}

float CloudSunScatteringWithDiffusion(
    float lightOpticalDepth,
    float forwardG,
    float backG,
    float backWeight,
    float cosTheta,
    float diffusion,
    float kappa,
    float similarity)
{
    const float octaves = CloudSunScattering(lightOpticalDepth, forwardG, backG, backWeight, cosTheta);
    if (diffusion <= 0.0f)
    {
        return octaves;
    }
    const float single = CloudPhase(forwardG, backG, backWeight, cosTheta) * std::exp(-lightOpticalDepth);
    const float diffused = single + CloudDiffuseScattering(lightOpticalDepth, kappa, similarity);
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
