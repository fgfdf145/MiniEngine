#ifndef NORMAL_MAP_GLSL
#define NORMAL_MAP_GLSL

// Tangent-space normal from a normal map texel. Only red and green are read: BC5 stores nothing
// else, and for an uncompressed map z is exactly what a unit vector implies, so the compressed and
// uncompressed paths agree without a shader variant.
vec3 DecodeNormalMap(vec4 texel)
{
    vec2 xy = texel.rg * 2.0 - 1.0;
    return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

#endif
