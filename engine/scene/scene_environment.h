#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace me
{

// What fills the pixels no geometry covers, and whether the atmosphere tints the sun and fogs the
// scene. The values reach the shaders through the camera uniform block and must match the
// ENVIRONMENT_* constants in shaders/vulkan/atmosphere_common.glsl.
enum class EnvironmentMode : uint32_t
{
    // The flat viewport background, standing for no physical light.
    None = 0,
    Atmosphere = 1,
    Hdri = 2
};

// Top-of-atmosphere illuminance of the startup scene's sun, in lux.
inline constexpr float kDefaultSunIlluminanceLux = 120000.0f;

// The parts of Hillaire 2020's Earth atmosphere the editor exposes. Radii and base coefficients
// are fixed; see engine/renderer/atmosphere.h.
struct AtmosphereSettings
{
    glm::vec3 groundAlbedo{0.3f, 0.3f, 0.3f};
    // Multipliers on the base Rayleigh scattering, Mie scattering and extinction, and ozone
    // absorption coefficients.
    float rayleighDensityScale = 1.0f;
    float mieDensityScale = 1.0f;
    // Cornette-Shanks g.
    float mieAnisotropy = 0.8f;
    float ozoneDensityScale = 1.0f;
    // Multiplies every view distance before the aerial perspective lookup, so an editor-sized
    // scene can show the haze the real atmosphere only builds up over kilometres.
    float aerialPerspectiveDistanceScale = 1.0f;
    float sunAngularDiameterDegrees = 0.545f;

    bool operator==(const AtmosphereSettings&) const = default;
};

struct HdriSettings
{
    // An equirectangular .hdr or .exr, referenced like a model: path plus asset uuid.
    std::string path;
    std::string uuid;
    // cd/m^2 per unit of texel value.
    float intensity = 1000.0f;
    // Turns the map about +Y.
    float rotationDegrees = 0.0f;

    bool operator==(const HdriSettings&) const = default;
};

struct SceneEnvironment
{
    EnvironmentMode mode = EnvironmentMode::None;
    AtmosphereSettings atmosphere;
    HdriSettings hdri;

    bool operator==(const SceneEnvironment&) const = default;
};
}
