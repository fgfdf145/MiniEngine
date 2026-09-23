#ifndef VBAO_COMMON_GLSL
#define VBAO_COMMON_GLSL

// Shared by vbao_trace.comp and vbao_resolve.comp. Include scene_common.glsl first.

// Must match AoPushConstants in engine/renderer/vulkan/ao_pass.cpp.
layout(push_constant) uniform AoConstants
{
    vec2 extent;
    vec2 invExtent;
    float radius;         // metres
    float thickness;      // metres
    float maxPixelRadius; // screen-space cap on the search, pixels
    float unused;
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

#endif
