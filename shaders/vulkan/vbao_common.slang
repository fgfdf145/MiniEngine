#ifndef VBAO_COMMON_SLANG
#define VBAO_COMMON_SLANG

// Shared by vbao_trace.comp and vbao_resolve.comp, and by the indirect diffuse passes that march the
// same bitmask (gi_trace.comp, gi_resolve.comp). Include scene_common.slang first.

// Must match AoPushConstants in engine/renderer/vulkan/ao_pass.cpp and GiPushConstants in
// engine/renderer/vulkan/gi_pass.cpp.
struct AoConstants
{
    float2 extent;
    float2 invExtent;
    float radius;         // metres
    float thickness;      // metres
    float maxPixelRadius; // screen-space cap on the search, pixels
    float strength;       // the indirect diffuse passes' multiplier on the bounced light; AO ignores it
    uint sliceCount;
    uint stepCount;
    uint frameIndex;
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<AoConstants> aoConstants;

static const uint AO_FLAG_ENABLED = 1u;
static const uint AO_FLAG_SPATIAL = 2u;
static const uint AO_FLAG_TEMPORAL = 4u;
static const uint AO_FLAG_HISTORY_VALID = 8u;
// Hardware ray tracing (rt_occlusion.comp): the AO is traced rather than marched, and the DDGI probe
// occlusion is traced into AoRaw's g. The GI passes never set them.
static const uint AO_FLAG_RAY_TRACED_AO = 16u;
static const uint AO_FLAG_PROBE_OCCLUSION = 32u;

// View-space depth (negative in front of the camera) from a 0..1 depth sample, for glm's
// perspectiveRH_ZO: clip.z = P22 * z + P32, clip.w = -z.
float ViewZFromDepth(float depth)
{
    return -ubo.proj[2][3] / (depth + ubo.proj[2][2]);
}

// View-space position of a pixel centre. uv has its origin at the top left; the Y flip lives in
// proj[1][1], so it falls out of the division without a sign here.
float3 ViewPositionFromDepth(float2 uv, float depth)
{
    float viewZ = ViewZFromDepth(depth);
    float2 ndc = uv * 2.0 - 1.0;
    return float3(ndc.x * -viewZ / ubo.proj[0][0], ndc.y * -viewZ / ubo.proj[1][1], viewZ);
}

// The half-resolution traces (vbao_trace.comp, gi_trace.comp) trace one full-resolution pixel of
// each 2x2 block. With the temporal filter on the pixel cycles through the block over four frames,
// so the history sees every pixel; without it the pattern holds still rather than shimmer. The
// resolves call this too, to find where each half-resolution sample came from.
int2 HalfResSourcePixel(int2 halfPixel)
{
    const int2 kOffsets[4] = {int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1)};
    uint index = (aoConstants.flags & AO_FLAG_TEMPORAL) != 0u ? aoConstants.frameIndex % 4u : 0u;
    return min(halfPixel * 2 + kOffsets[index], int2(aoConstants.extent) - 1);
}

int2 HalfResExtent()
{
    return (int2(aoConstants.extent) + 1) / 2;
}

#endif
