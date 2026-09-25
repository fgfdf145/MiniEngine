#pragma once

#include <engine/asset/texture_loader.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace me
{

// The DFG table set 0 binding 9 samples: 64 x 64, 2048 samples per texel. The correlated visibility
// has more variance under GGX importance sampling than Schlick-Smith had, and 512 samples left up
// to 0.013 of noise at grazing angles.
inline constexpr uint32_t kEnvironmentBrdfLutSize = 64;
inline constexpr uint32_t kEnvironmentBrdfSampleCount = 2048;

// The split-sum environment BRDF (Karis 2013): specular albedo = F0 * A + B for a GGX lobe of
// this roughness seen at this N.V, integrated with sampleCount GGX importance samples and the
// height-correlated Smith visibility the direct lights use (alpha = roughness^2).
glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount);

// The factor that adds back the energy a single-scattering GGX lobe loses to light bouncing more
// than once between microfacets (Fdez-Aguera 2019, in Filament's form): 1 + F0 (1 / (A + B) - 1),
// with (A, B) = IntegrateEnvironmentBrdf's result. A + B is the lobe's directional albedo for
// F0 = 1, so a perfect conductor reflects exactly everything once its specular term is scaled by
// this; rough metals gain the most, dielectrics almost nothing. pbr_common.glsl's
// SpecularEnergyCompensation is the same formula.
glm::vec3 SpecularEnergyCompensation(const glm::vec3& f0, const glm::vec2& environmentBrdf);

// The directional albedo of the sheen lobe (KHR_materials_sheen, Filament's model): the integral of
// D_Charlie * V_Neubelt * N.L over the hemisphere for a white sheen at this perceptual roughness,
// seen at this N.V. pbr_common.glsl scales the base by 1 - max(sheenColor) * this and weights the
// sheen's ambient by it. Estimated with sampleCount half vectors spread uniformly over the
// hemisphere (Charlie's lobe is too broad for importance sampling to pay off). Not clamped: the
// model exceeds 1 for smooth sheen at grazing angles; BuildEnvironmentBrdfLut clamps what it stores.
float IntegrateSheenAlbedo(float roughness, float NdV, uint32_t sampleCount);

// The table: texel (x, y) holds (A, B, sheen albedo, 1) for N.V = (x + 0.5) / size and roughness =
// (y + 0.5) / size, rows top-down like every texture.
FloatTextureData BuildEnvironmentBrdfLut(uint32_t size, uint32_t sampleCount);
}
