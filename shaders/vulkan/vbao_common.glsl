#ifndef VBAO_COMMON_GLSL
#define VBAO_COMMON_GLSL

// Shared by vbao_trace.comp and vbao_resolve.comp, and by the indirect diffuse passes that march the
// same bitmask (gi_trace.comp, gi_resolve.comp). Include scene_common.glsl first.

// Must match AoPushConstants in engine/renderer/vulkan/ao_pass.cpp and GiPushConstants in
// engine/renderer/vulkan/gi_pass.cpp.
layout(push_constant) uniform AoConstants
{
    vec2 extent;
    vec2 invExtent;
    float radius;         // metres
    float thickness;      // metres
    float maxPixelRadius; // screen-space cap on the search, pixels
    float strength;       // the indirect diffuse passes' multiplier on the bounced light; AO ignores it
    uint sliceCount;
    uint stepCount;
    uint frameIndex;
    uint flags;
}
aoConstants;

const uint AO_FLAG_ENABLED = 1u;
const uint AO_FLAG_SPATIAL = 2u;
const uint AO_FLAG_TEMPORAL = 4u;
const uint AO_FLAG_HISTORY_VALID = 8u;
// Hardware ray tracing (rt_occlusion.comp): the AO is traced rather than marched, and the DDGI probe
// occlusion is traced into AoRaw's g. The GI passes never set them.
const uint AO_FLAG_RAY_TRACED_AO = 16u;
const uint AO_FLAG_PROBE_OCCLUSION = 32u;

// View-space depth (negative in front of the camera) from a 0..1 depth sample, for glm's
// perspectiveRH_ZO: clip.z = P22 * z + P32, clip.w = -z.
float ViewZFromDepth(float depth)
{
    return -ubo.proj[3][2] / (depth + ubo.proj[2][2]);
}

// View-space position of a pixel centre. uv has its origin at the top left; the Y flip lives in
// proj[1][1], so it falls out of the division without a sign here.
vec3 ViewPositionFromDepth(vec2 uv, float depth)
{
    float viewZ = ViewZFromDepth(depth);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * -viewZ / ubo.proj[0][0], ndc.y * -viewZ / ubo.proj[1][1], viewZ);
}

// The half-resolution traces (vbao_trace.comp, gi_trace.comp) trace one full-resolution pixel of
// each 2x2 block. With the temporal filter on the pixel cycles through the block over four frames,
// so the history sees every pixel; without it the pattern holds still rather than shimmer. The
// resolves call this too, to find where each half-resolution sample came from.
ivec2 HalfResSourcePixel(ivec2 halfPixel)
{
    const ivec2 kOffsets[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    uint index = (aoConstants.flags & AO_FLAG_TEMPORAL) != 0u ? aoConstants.frameIndex % 4u : 0u;
    return min(halfPixel * 2 + kOffsets[index], ivec2(aoConstants.extent) - 1);
}

ivec2 HalfResExtent()
{
    return (ivec2(aoConstants.extent) + 1) / 2;
}

#endif
