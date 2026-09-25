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

// The shading flags in GB2.a: kShadingFlag* in engine/renderer/material.h. Stored as flags / 255
// in the UNORM channel, so they round-trip exactly below 256. The lighting pass reads only the
// targets the flags name.
// GB6.rg: the coat's factor and roughness.
const uint SHADING_FLAG_CLEARCOAT = 1u;
// GB7: the sheen's colour and roughness.
const uint SHADING_FLAG_SHEEN = 2u;
// GB6.ba: the anisotropy's angle over pi, in the frame OrthonormalTangent / OrthonormalBitangent
// build from the shading normal, and its strength.
const uint SHADING_FLAG_ANISOTROPY = 4u;
// GB5: sqrt of the dielectric's F0 in rgb, its F90 in a.
const uint SHADING_FLAG_SPECULAR = 8u;
// The velocity target's .ba: the coat's own normal, octahedral.
const uint SHADING_FLAG_COAT_NORMAL = 16u;
// Shaded by the forward pass; the lighting pass skips the pixel.
const uint SHADING_FLAG_FORWARD = 32u;

float EncodeShadingFlags(uint flags)
{
    return float(flags) / 255.0;
}

uint DecodeShadingFlags(float encoded)
{
    return uint(encoded * 255.0 + 0.5);
}

#endif
