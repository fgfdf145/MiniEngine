#pragma once

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

#include <optional>

namespace me
{

// The ozone layer's tent profile (Hillaire 2020): density 1 at 25 km, falling linearly to 0 at 10
// and 40 km. Mirrored by OZONE_* in shaders/vulkan/atmosphere_common.glsl.
inline constexpr float kOzoneCenterAltitudeKm = 25.0f;
inline constexpr float kOzoneHalfWidthKm = 15.0f;

// Hillaire 2020's Earth atmosphere with the scene's scales applied, in kilometres. Everything the
// LUT shaders need comes from here through EnvironmentUniformData, so no coefficient is written
// twice.
struct AtmosphereParameters
{
    float bottomRadiusKm = 6360.0f;
    float topRadiusKm = 6460.0f;
    glm::vec3 rayleighScattering{5.802e-3f, 13.558e-3f, 33.1e-3f};
    float rayleighScaleHeightKm = 8.0f;
    float mieScattering = 3.996e-3f;
    float mieExtinction = 4.40e-3f;
    float mieScaleHeightKm = 1.2f;
    float mieAnisotropy = 0.8f;
    glm::vec3 ozoneAbsorption{0.650e-3f, 1.881e-3f, 0.085e-3f};
    glm::vec3 groundAlbedo{0.3f};
    float sunAngularDiameterDegrees = 0.545f;
    float aerialPerspectiveDistanceScale = 1.0f;

    bool operator==(const AtmosphereParameters&) const = default;
};

// Applies the settings' scales to the Earth defaults. Every setting is clamped to the range the
// editor offers, so a value from a hand-edited scene cannot reach the shaders.
AtmosphereParameters BuildAtmosphereParameters(const AtmosphereSettings& settings);

// Per-km extinction at an altitude above the ground.
glm::vec3 ComputeExtinction(const AtmosphereParameters& p, float altitudeKm);

// Transmittance from a point at this altitude (km above the ground) along a direction with this
// cos zenith, out to the top of the atmosphere. Zero when the ray hits the ground; one when it
// never enters the atmosphere. Integrates the same medium as the transmittance LUT shader.
glm::vec3 ComputeTransmittanceToSpace(const AtmosphereParameters& p, float altitudeKm, float cosZenith);

// The camera relative to the planet centre, in km: world metres / 1000, with world y = 0 on the
// ground. The altitude is held in [0.5 m, top - 1 km], the range the LUTs are built for.
glm::vec3 ToAtmosphereCameraPositionKm(const AtmosphereParameters& p, const glm::vec3& cameraPositionMeters);

struct AtmosphereSun
{
    // Unit vector from the scene toward the sun.
    glm::vec3 directionToSun{0.0f, 1.0f, 0.0f};
    // Top-of-atmosphere illuminance in lux: the light's colour times its intensity.
    glm::vec3 illuminance{0.0f};
};

// The environment block at the end of CameraBuffer in shaders/vulkan/scene_common.glsl, member for
// member. Every member is a vec4 so the C++ layout is the std140 layout.
struct EnvironmentUniformData
{
    glm::vec4 sunDirectionAndMode{0.0f, 1.0f, 0.0f, 0.0f}; // xyz toward the sun, w EnvironmentMode
    glm::vec4 sunIlluminance{0.0f};                        // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    glm::vec4 rayleighScattering{0.0f};                    // rgb per km, w scale height km
    glm::vec4 mieParameters{0.0f};                         // x scattering per km, y extinction per km, z scale height km, w g
    glm::vec4 ozoneAbsorption{0.0f};                       // rgb per km
    glm::vec4 groundAlbedo{0.0f};                          // rgb
    glm::vec4 radii{0.0f};                                 // x bottom km, y top km, z aerial perspective distance scale
    glm::vec4 cameraPositionKm{0.0f};                      // xyz, see ToAtmosphereCameraPositionKm
    glm::vec4 hdriParameters{0.0f};                        // x intensity, y rotation in turns
};
static_assert(sizeof(EnvironmentUniformData) == 9 * 16, "EnvironmentUniformData must stay nine vec4s");

// mode is the mode the frame renders with, which differs from environment.mode while an HDRI is
// still loading. With no sun the illuminance is zero and the sky is black.
EnvironmentUniformData BuildEnvironmentUniformData(
    EnvironmentMode mode,
    const SceneEnvironment& environment,
    const AtmosphereParameters& p,
    const std::optional<AtmosphereSun>& sun,
    const glm::vec3& cameraPositionMeters);
}
