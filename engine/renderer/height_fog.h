#pragma once

#include <engine/scene/scene_environment.h>

#include <glm/glm.hpp>

namespace me
{

// Exponential height fog (docs/design/2026-09-28-height-fog-design.md): extinction
// sigma(y) = density * exp(-(y - fogHeight) * falloff) per metre, the same in r, g and b. The
// shaders mirror these functions line for line in shaders/vulkan/height_fog.slang.

// The height exponent -(y - fogHeight) * falloff is held at or below this, so a camera far below
// the fog height cannot overflow.
inline constexpr float kHeightFogMaxExponent = 80.0f;

// The settings held to the ranges the editor offers, as BuildEnvironmentUniformData uploads them.
HeightFogSettings ClampHeightFogSettings(const HeightFogSettings& settings);

// Optical depth along cameraPosition + t * direction (unit) from startDistance to distance.
// Zero when the surface is closer than startDistance.
float HeightFogOpticalDepth(const HeightFogSettings& settings, const glm::vec3& cameraPosition, const glm::vec3& direction, float distance);

// The same out to infinity: finite for rays that climb, infinite for level and falling rays.
float HeightFogSkyOpticalDepth(const HeightFogSettings& settings, const glm::vec3& cameraPosition, const glm::vec3& direction);

// Henyey-Greenstein phase function per steradian; cosTheta is 1 looking into the light.
float HenyeyGreenstein(float g, float cosTheta);
}
