#pragma once

#include <array>
#include <cstdint>

namespace me
{

// The linearly transformed cosine tables of the area lights (Heitz et al. 2016), fitted to this
// engine's specular lobe by tools/ltc_fit/ltc_fit.cpp. Row r holds roughness (r + 0.5) / size,
// column c holds sqrt(1 - N.V) = (c + 0.5) / size, both sampled at texel centres.
inline constexpr uint32_t kLtcTableSize = 64;

// Per texel the inverse matrix, (m00, m02, m20, m22) of mat3(vec3(m00, 0, m02), vec3(0, 1, 0),
// vec3(m20, 0, m22)) in GLSL's column order.
extern const std::array<float, kLtcTableSize * kLtcTableSize * 4> kLtcInverseMatrices;

// Per texel (norm, fresnel, 0, 0): the lobe's albedo for F = 1, and the same integral weighted by
// (1 - V.H)^5. With Schlick's Fresnel the lobe reflects F0 * norm + (1 - F0) * fresnel.
extern const std::array<float, kLtcTableSize * kLtcTableSize * 4> kLtcAmplitudes;
}
