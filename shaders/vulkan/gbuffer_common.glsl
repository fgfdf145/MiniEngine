#ifndef GBUFFER_COMMON_GLSL
#define GBUFFER_COMMON_GLSL

// Octahedral normal encoding. A unit vector is projected onto the octahedron
// |x| + |y| + |z| = 1 and the lower hemisphere is folded over the upper one's corners, leaving
// two components in [-1, 1]. GB1 is R16G16B16A16_SFLOAT and holds two such pairs, the shading
// normal in .rg and the geometric normal in .ba. Half floats store that range directly: there is
// no 0.5 * n + 0.5 remap, and adding one would only discard precision.

vec2 OctahedralWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 EncodeNormalOctahedral(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : OctahedralWrap(n.xy);
}

vec3 DecodeNormalOctahedral(vec2 encoded)
{
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// The shading model id in GB2.a: ShadingModel in engine/renderer/material.h. Stored as id / 255 in
// the UNORM channel, so it round-trips exactly for every id below 256.
const uint SHADING_MODEL_DEFAULT_LIT = 0u;
// GB5.r = clearcoat factor, GB5.g = clearcoat roughness.
const uint SHADING_MODEL_CLEARCOAT = 1u;
// GB5.rgb = sheen colour, GB5.a = sheen roughness.
const uint SHADING_MODEL_SHEEN = 2u;

float EncodeShadingModel(uint shadingModel)
{
    return float(shadingModel) / 255.0;
}

uint DecodeShadingModel(float encoded)
{
    return uint(encoded * 255.0 + 0.5);
}

#endif
