#ifndef SSR_LOBE_SLANG
#define SSR_LOBE_SLANG

// The lobe a pixel's screen-space ray follows (SsrTracesCoat): its normal and roughness, as the
// lighting pass reads them from the G-buffer. The includer declares normalTexture (GB1),
// surfaceTexture (GB2), coatTexture (GB6) and velocityTexture (whose .ba holds a mapped coat's
// normal), all read with Load.

#include "ssr_common.slang"

struct SsrLobe
{
    float3 normal;
    float roughness;
};

SsrLobe ReadSsrLobe(int2 pixel)
{
    float4 normals = normalTexture.Load(int3(pixel, 0));
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    SsrLobe lobe;
    lobe.normal = DecodeNormalOctahedral(normals.rg);
    lobe.roughness = clamp(surface.g, 0.04, 1.0);
    uint flags = DecodeShadingFlags(surface.a);
    if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        float2 coat = coatTexture.Load(int3(pixel, 0)).rg;
        float coatRoughness = clamp(coat.g, 0.04, 1.0);
        if (SsrTracesCoat(coat.r, coatRoughness, lobe.roughness))
        {
            // The coat's own normal when it has a map, else the geometric normal (GB1.ba), as
            // deferred_lighting.frag reads it.
            lobe.normal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u ? DecodeNormalOctahedral(velocityTexture.Load(int3(pixel, 0)).ba)
                                                                    : DecodeNormalOctahedral(normals.ba);
            lobe.roughness = coatRoughness;
        }
    }
    return lobe;
}

#endif
