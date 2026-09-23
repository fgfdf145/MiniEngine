#include "atmosphere.h"

#include <algorithm>
#include <cmath>

namespace me
{

AtmosphereParameters BuildAtmosphereParameters(const AtmosphereSettings& settings)
{
    AtmosphereParameters p{};
    const float rayleigh = std::clamp(settings.rayleighDensityScale, 0.0f, 10.0f);
    const float mie = std::clamp(settings.mieDensityScale, 0.0f, 10.0f);
    const float ozone = std::clamp(settings.ozoneDensityScale, 0.0f, 10.0f);
    p.rayleighScattering *= rayleigh;
    p.mieScattering *= mie;
    p.mieExtinction *= mie;
    p.ozoneAbsorption *= ozone;
    p.mieAnisotropy = std::clamp(settings.mieAnisotropy, 0.0f, 0.99f);
    p.groundAlbedo = glm::clamp(settings.groundAlbedo, glm::vec3(0.0f), glm::vec3(1.0f));
    p.sunAngularDiameterDegrees = std::clamp(settings.sunAngularDiameterDegrees, 0.1f, 5.0f);
    p.aerialPerspectiveDistanceScale = std::clamp(settings.aerialPerspectiveDistanceScale, 0.0f, 10000.0f);
    return p;
}

glm::vec3 ComputeExtinction(const AtmosphereParameters& p, float altitudeKm)
{
    const float altitude = std::max(altitudeKm, 0.0f);
    const float rayleighDensity = std::exp(-altitude / p.rayleighScaleHeightKm);
    const float mieDensity = std::exp(-altitude / p.mieScaleHeightKm);
    const float ozoneDensity = std::max(0.0f, 1.0f - std::fabs(altitude - kOzoneCenterAltitudeKm) / kOzoneHalfWidthKm);
    return p.rayleighScattering * rayleighDensity + glm::vec3(p.mieExtinction * mieDensity) + p.ozoneAbsorption * ozoneDensity;
}

glm::vec3 ComputeTransmittanceToSpace(const AtmosphereParameters& p, float altitudeKm, float cosZenith)
{
    const float r = p.bottomRadiusKm + std::max(altitudeKm, 0.0f);
    const float mu = std::clamp(cosZenith, -1.0f, 1.0f);
    // Below the horizon of the ground sphere: the ray ends on the planet.
    const float groundDiscriminant = r * r * (mu * mu - 1.0f) + p.bottomRadiusKm * p.bottomRadiusKm;
    if (mu < 0.0f && groundDiscriminant >= 0.0f)
    {
        return glm::vec3(0.0f);
    }
    const float topDiscriminant = r * r * (mu * mu - 1.0f) + p.topRadiusKm * p.topRadiusKm;
    if (topDiscriminant < 0.0f)
    {
        return glm::vec3(1.0f);
    }
    const float distance = -r * mu + std::sqrt(topDiscriminant);
    if (distance <= 0.0f)
    {
        return glm::vec3(1.0f);
    }

    constexpr int kSteps = 500;
    const float dt = distance / static_cast<float>(kSteps);
    glm::vec3 opticalDepth(0.0f);
    for (int step = 0; step < kSteps; ++step)
    {
        const float t = (static_cast<float>(step) + 0.5f) * dt;
        const float height = std::sqrt(r * r + t * t + 2.0f * r * mu * t) - p.bottomRadiusKm;
        opticalDepth += ComputeExtinction(p, height) * dt;
    }
    return glm::exp(-opticalDepth);
}

glm::vec3 ToAtmosphereCameraPositionKm(const AtmosphereParameters& p, const glm::vec3& cameraPositionMeters)
{
    const float altitude = std::clamp(cameraPositionMeters.y * 0.001f, 0.0005f, p.topRadiusKm - p.bottomRadiusKm - 1.0f);
    return glm::vec3(cameraPositionMeters.x * 0.001f, p.bottomRadiusKm + altitude, cameraPositionMeters.z * 0.001f);
}

EnvironmentUniformData BuildEnvironmentUniformData(
    EnvironmentMode mode,
    const SceneEnvironment& environment,
    const AtmosphereParameters& p,
    const std::optional<AtmosphereSun>& sun,
    const glm::vec3& cameraPositionMeters)
{
    EnvironmentUniformData data{};
    const glm::vec3 directionToSun = sun.has_value() ? glm::normalize(sun->directionToSun) : glm::vec3(0.0f, 1.0f, 0.0f);
    data.sunDirectionAndMode = glm::vec4(directionToSun, static_cast<float>(static_cast<uint32_t>(mode)));
    data.sunIlluminance = glm::vec4(
        sun.has_value() ? sun->illuminance : glm::vec3(0.0f),
        std::cos(glm::radians(p.sunAngularDiameterDegrees * 0.5f)));
    data.rayleighScattering = glm::vec4(p.rayleighScattering, p.rayleighScaleHeightKm);
    data.mieParameters = glm::vec4(p.mieScattering, p.mieExtinction, p.mieScaleHeightKm, p.mieAnisotropy);
    data.ozoneAbsorption = glm::vec4(p.ozoneAbsorption, 0.0f);
    data.groundAlbedo = glm::vec4(p.groundAlbedo, 0.0f);
    data.radii = glm::vec4(p.bottomRadiusKm, p.topRadiusKm, p.aerialPerspectiveDistanceScale, 0.0f);
    data.cameraPositionKm = glm::vec4(ToAtmosphereCameraPositionKm(p, cameraPositionMeters), 0.0f);
    data.hdriParameters = glm::vec4(
        std::max(environment.hdri.intensity, 0.0f),
        environment.hdri.rotationDegrees / 360.0f,
        0.0f,
        0.0f);
    return data;
}
}
