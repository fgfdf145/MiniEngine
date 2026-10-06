#ifndef SSR_LOBE_GLSL
#define SSR_LOBE_GLSL

// The lobe a pixel's screen-space ray follows (SsrTracesCoat): its normal and roughness, as the
// lighting pass reads them from the G-buffer. The includer declares normalTexture (GB1),
// surfaceTexture (GB2), coatTexture (GB6) and velocityTexture (whose .ba holds a mapped coat's
// normal), all read with texelFetch.

#include "ssr_common.glsl"

struct SsrLobe
{
    vec3 normal;
    float roughness;
};

SsrLobe ReadSsrLobe(ivec2 pixel)
{
    vec4 normals = texelFetch(normalTexture, pixel, 0);
    vec4 surface = texelFetch(surfaceTexture, pixel, 0);
    SsrLobe lobe;
    lobe.normal = DecodeNormalOctahedral(normals.rg);
    lobe.roughness = clamp(surface.g, 0.04, 1.0);
    uint flags = DecodeShadingFlags(surface.a);
    if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        vec2 coat = texelFetch(coatTexture, pixel, 0).rg;
        float coatRoughness = clamp(coat.g, 0.04, 1.0);
        if (SsrTracesCoat(coat.r, coatRoughness, lobe.roughness))
        {
            // The coat's own normal when it has a map, else the geometric normal (GB1.ba), as
            // deferred_lighting.frag reads it.
            lobe.normal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u ? DecodeNormalOctahedral(texelFetch(velocityTexture, pixel, 0).ba)
                                                                    : DecodeNormalOctahedral(normals.ba);
            lobe.roughness = coatRoughness;
        }
    }
    return lobe;
}

#endif
