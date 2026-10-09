#pragma once

#include "spherical_harmonics.h"

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>

namespace me
{

// The ozone layer's tent profile (Hillaire 2020): density 1 at 25 km, falling linearly to 0 at 10
// and 40 km. Mirrored by OZONE_* in shaders/vulkan/atmosphere_common.slang.
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

// The environment block at the end of CameraBuffer in shaders/vulkan/scene_common.slang, member for
// member. Every member is a vec4 so the C++ layout is the std140 layout.
struct EnvironmentUniformData
{
    glm::vec4 sunDirectionAndMode{0.0f, 1.0f, 0.0f, 0.0f}; // xyz toward the sun, w EnvironmentMode
    glm::vec4 sunIlluminance{0.0f};                        // rgb lux at the top of the atmosphere, w cos(sun angular radius)
    glm::vec4 rayleighScattering{0.0f};                    // rgb per km, w scale height km
    glm::vec4 mieParameters{0.0f};                         // x scattering per km, y extinction per km, z scale height km, w g
    glm::vec4 ozoneAbsorption{0.0f};                       // rgb per km
    glm::vec4 groundAlbedo{0.0f};                          // rgb, w 1 when the ground plane is drawn
    glm::vec4 radii{0.0f};                                 // x bottom km, y top km, z aerial perspective distance scale, w 1 for a seamless horizon
    glm::vec4 cameraPositionKm{0.0f};                      // xyz, see ToAtmosphereCameraPositionKm
    glm::vec4 hdriParameters{0.0f};                        // x intensity, y rotation in turns
    // The HDRI's radiance SH, rotated and scaled by its intensity; xyz used.
    glm::vec4 hdriIrradianceSh[9]{};
    // Exponential height fog (height_fog.h), clamped: x density per m (0 when off or outside the
    // Atmosphere mode), y falloff per m, z fog height m, w start distance m.
    glm::vec4 heightFogDensity{0.0f};
    glm::vec4 heightFogColor{0.0f}; // rgb albedo, w max opacity
    // x Henyey-Greenstein g; yzw the sun's illuminance at the camera (through the atmosphere, zero
    // once it has set) times the albedo, per frame on the CPU rather than per pixel.
    glm::vec4 heightFogParams{0.0f};
    // Volumetric clouds (volumetric_clouds.h), clamped, in kilometres: x base altitude, y thickness,
    // z the coverage as CloudCoverageOffset, w extinction per km where the cloud is fully dense (0
    // when off, at coverage 0 or outside the Atmosphere mode).
    glm::vec4 cloudLayer{0.0f};
    // Frequencies per km (one over the tile sizes) of the large billows, the small billows and the
    // plume map; w the billow strength.
    glm::vec4 cloudScales{0.0f};
    glm::vec4 cloudPhase{0.0f}; // x forward g, y back g, z back weight, w single-scattering albedo
    // x ambient scale, y haze distance km, z CloudDeckWeight of the coverage, w the frame's index
    // for the march's jitter.
    // w changes every frame, so the environment probe's CaptureKey leaves it out.
    glm::vec4 cloudParams{0.0f};
    // rgb: the moonless night sky's luminance, cd/m^2, added to the sky above the horizon
    // (TimeOfDaySettings::nightSkyLuminance; zero with the clock off).
    glm::vec4 nightSky{0.0f};
    // The clouds' diffusion and ambient occlusion (volumetric_clouds.h): x diffusion, y ambient
    // occlusion, z the diffusion field's decay kappa, w the dual lobe's mean cosine.
    glm::vec4 cloudLighting{0.0f};
    // The clouds' motion (CloudMotion, SetCloudMotion), as offsets in tiles so they keep their
    // precision however long the clock runs: xyz how far the large billows have moved (the wind
    // and their rise) in their tiles, w the plume map's u offset; the same for the small billows,
    // w the plume map's v offset.
    glm::vec4 cloudShapeMotion{0.0f};
    glm::vec4 cloudDetailMotion{0.0f};
    // Each plume scale's phase of life, CloudLifePhases.
    glm::vec4 cloudLife{0.0f};
    // xy this frame's wind displacement of the layer, world x, z in metres, for the reprojection; w
    // the clouds' clock in seconds. All of it changes every frame: CaptureKey leaves it out.
    glm::vec4 cloudMotionStep{0.0f};
    // The scene's wind (engine/scene/wind.h): xyz the world direction it blows toward, w its speed
    // at 10 m, m/s; shaders scale it with height as WindSpeedAt does.
    glm::vec4 wind{0.0f};
};
inline constexpr size_t kEnvironmentUniformVec4Count = 32;
static_assert(sizeof(EnvironmentUniformData) == kEnvironmentUniformVec4Count * 16, "EnvironmentUniformData must stay thirty-two vec4s");

// mode is the mode the frame renders with, which differs from environment.mode while an HDRI is
// still loading. hdriSh is the loaded HDRI's unrotated radiance SH, or null. With no sun the illuminance is zero and the sky is black.
EnvironmentUniformData BuildEnvironmentUniformData(
    EnvironmentMode mode,
    const SceneEnvironment& environment,
    const AtmosphereParameters& p,
    const std::optional<AtmosphereSun>& sun,
    const glm::vec3& cameraPositionMeters,
    const ShCoefficients* hdriSh);

struct CloudMotion;

// Puts the clouds' motion into data: the billows' and the plume map's offsets in their tiles, the
// plume lives and this frame's step (the tile sizes from environment.clouds, clamped).
void SetCloudMotion(EnvironmentUniformData& data, const SceneEnvironment& environment, const CloudMotion& motion);
}
