#pragma once

namespace me
{

// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019, as Filament ships it). Must match
// shaders/vulkan/specular_aa.glsl.
inline constexpr float kSpecularAAVariance = 0.15f;
inline constexpr float kSpecularAAThreshold = 0.2f;

// Widens a lobe by how much its normal varies across the pixel. normalDerivativeLengthSquared is
// |dN/dx|^2 + |dN/dy|^2 of the unit normal across one pixel. The kernel
// min(2 * variance * that, threshold) is added to alpha^2, alpha being the perceptual roughness
// squared; a flat surface keeps its roughness exactly.
float FilterRoughnessForSpecularAA(float perceptualRoughness, float normalDerivativeLengthSquared);
}
