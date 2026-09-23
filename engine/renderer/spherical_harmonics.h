#pragma once

#include <engine/asset/texture_loader.h>

#include <glm/glm.hpp>

#include <array>

namespace me
{

// RGB radiance projected onto the nine real spherical harmonics up to band 2, in the Y-up basis of
// shaders/vulkan/spherical_harmonics.glsl (same order and constants): 0 Y00, 1 Y1-1 (z), 2 Y10 (y),
// 3 Y11 (x), 4 Y2-2 (xz), 5 Y2-1 (zy), 6 Y20 (3y^2 - 1), 7 Y21 (xy), 8 Y22 (x^2 - z^2).
using ShCoefficients = std::array<glm::vec3, 9>;

std::array<float, 9> EvaluateShBasis(const glm::vec3& direction);

// The world direction of equirectangular coordinates, the inverse of SampleEnvironmentMap in
// shaders/vulkan/atmosphere_sampling.glsl at zero rotation: u = 0.5 is -Z, 0.75 is +X, v = 0 is +Y.
glm::vec3 EquirectangularDirection(float u, float v);

// Projects every texel, weighted by its solid angle. Throws std::runtime_error for invalid data.
ShCoefficients ProjectEquirectangular(const FloatTextureData& map);

// Turns the function the coefficients describe by radians about +Y. Which way a positive angle
// turns is pinned against the renderer by ShForHdriRotation and its test, not stated here.
ShCoefficients RotateShAboutY(const ShCoefficients& sh, float radians);

// The SH of a map drawn with HdriSettings::rotationDegrees, from the SH of the map itself.
ShCoefficients ShForHdriRotation(const ShCoefficients& sh, float rotationDegrees);

// Irradiance for a surface facing normal: the radiance convolved with the clamped cosine.
glm::vec3 EvaluateShIrradiance(const ShCoefficients& sh, const glm::vec3& normal);
}
