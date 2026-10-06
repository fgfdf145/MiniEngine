// The ray traced sun shadow's three shaders (VulkanRtShadowPass): the trace, the temporal filter and
// the spatial filter share their push constants and their pass set, which is set RT_SHADOW_SET (2 in
// the trace, whose set 1 is the ray scene; 1 in the other two). Include after scene_common.glsl.

#ifndef RT_SHADOW_COMMON_GLSL
#define RT_SHADOW_COMMON_GLSL

#ifndef RT_SHADOW_SET
#define RT_SHADOW_SET 1
#endif

// Must match RtShadowPushConstants in engine/renderer/vulkan/rt_shadow_pass.cpp.
layout(push_constant) uniform RtShadowConstants
{
    vec2 extent;
    vec2 invExtent;
    uint frameIndex;
    uint flags;
    float unused0;
    float unused1;
}
rtShadow;

const uint RT_SHADOW_FLAG_ENABLED = 1u;
const uint RT_SHADOW_FLAG_DENOISE = 2u;
const uint RT_SHADOW_FLAG_HISTORY_VALID = 4u;

layout(set = RT_SHADOW_SET, binding = 0) uniform sampler2D rtShadowDepth;
layout(set = RT_SHADOW_SET, binding = 1) uniform sampler2D rtShadowNormal;
layout(set = RT_SHADOW_SET, binding = 2) uniform sampler2D rtShadowVelocity;
// x visibility (0 or 1), y the occluder's distance (0 when lit).
layout(set = RT_SHADOW_SET, binding = 3, rgba16f) uniform image2D rtShadowRaw;
// x accumulated visibility, y the view distance it was accumulated at, z sample count, w the
// occluders' mean distance.
layout(set = RT_SHADOW_SET, binding = 4) uniform sampler2D rtShadowHistoryRead;
layout(set = RT_SHADOW_SET, binding = 5, rgba16f) uniform writeonly image2D rtShadowHistoryWrite;
// x visibility the lighting reads, y 1 where the traced shadow ran.
layout(set = RT_SHADOW_SET, binding = 6, rgba16f) uniform writeonly image2D rtShadowOutput;
// The history the temporal filter just wrote, which the spatial filter reads.
layout(set = RT_SHADOW_SET, binding = 7, rgba16f) uniform readonly image2D rtShadowAccumulated;

// glm's perspectiveRH_ZO with reverse Z: view-space depth (positive) from a 0..1 depth sample.
float RtShadowViewDistance(float depth)
{
    return ubo.proj[3][2] / (depth + ubo.proj[2][2]);
}

vec3 RtShadowWorldPosition(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + 0.5) * rtShadow.invExtent;
    vec4 world = ubo.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

#endif
