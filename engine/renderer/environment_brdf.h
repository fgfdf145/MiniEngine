#pragma once

#include <engine/asset/texture_loader.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace me
{

// The DFG table set 0 binding 9 samples: 64 x 64, 512 samples per texel.
inline constexpr uint32_t kEnvironmentBrdfLutSize = 64;
inline constexpr uint32_t kEnvironmentBrdfSampleCount = 512;

// The split-sum environment BRDF (Karis 2013): specular albedo = F0 * A + B for a GGX lobe of
// this roughness seen at this N.V, integrated with sampleCount GGX importance samples and
// Schlick-Smith visibility with k = alpha / 2 (alpha = roughness^2), the remapping Karis uses for
// IBL.
glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount);

// The table: texel (x, y) holds (A, B, 0, 1) for N.V = (x + 0.5) / size and roughness =
// (y + 0.5) / size, rows top-down like every texture.
FloatTextureData BuildEnvironmentBrdfLut(uint32_t size, uint32_t sampleCount);
}
