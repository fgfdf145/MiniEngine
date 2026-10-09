// The ray traced sun shadow's three shaders (VulkanRtShadowPass): the trace, the temporal filter and
// the spatial filter share their push constants and their pass set, which is set RT_SHADOW_SET (2 in
// the trace, whose set 1 is the ray scene; 1 in the other two). Include after scene_common.slang.

#ifndef RT_SHADOW_COMMON_SLANG
#define RT_SHADOW_COMMON_SLANG

#include "d3d_register.slang"

#ifndef RT_SHADOW_SET
#define RT_SHADOW_SET 1
#endif

// Must match RtShadowPushConstants in engine/renderer/vulkan/rt_shadow_pass.cpp.
struct RtShadowConstants
{
    float2 extent;
    float2 invExtent;
    uint frameIndex;
    uint flags;
    float unused0;
    float unused1;
};
[[vk::push_constant]] ConstantBuffer<RtShadowConstants> rtShadow D3D_PUSH_CONSTANTS(RT_SHADOW_SET);

static const uint RT_SHADOW_FLAG_ENABLED = 1u;
static const uint RT_SHADOW_FLAG_DENOISE = 2u;
static const uint RT_SHADOW_FLAG_HISTORY_VALID = 4u;

[[vk::binding(0, RT_SHADOW_SET)]] Texture2D rtShadowDepth : D3D_REGISTER(t, 0, RT_SHADOW_SET);
[[vk::binding(1, RT_SHADOW_SET)]] Texture2D rtShadowNormal : D3D_REGISTER(t, 1, RT_SHADOW_SET);
[[vk::binding(2, RT_SHADOW_SET)]] Texture2D rtShadowVelocity : D3D_REGISTER(t, 2, RT_SHADOW_SET);
// x visibility (0 or 1), y the occluder's distance (0 when lit).
[[vk::binding(3, RT_SHADOW_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> rtShadowRaw : D3D_REGISTER(u, 3, RT_SHADOW_SET);
// x accumulated visibility, y the view distance it was accumulated at, z sample count, w the
// occluders' mean distance.
[[vk::binding(4, RT_SHADOW_SET)]] Texture2D rtShadowHistoryRead : D3D_REGISTER(t, 4, RT_SHADOW_SET);
[[vk::binding(68, RT_SHADOW_SET)]] SamplerState rtShadowHistoryReadSampler : D3D_REGISTER(s, 68, RT_SHADOW_SET);
[[vk::binding(5, RT_SHADOW_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> rtShadowHistoryWrite : D3D_REGISTER(u, 5, RT_SHADOW_SET);
// x visibility the lighting reads, y 1 where the traced shadow ran.
[[vk::binding(6, RT_SHADOW_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> rtShadowOutput : D3D_REGISTER(u, 6, RT_SHADOW_SET);
// The history the temporal filter just wrote, which the spatial filter reads.
[[vk::binding(7, RT_SHADOW_SET)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> rtShadowAccumulated : D3D_REGISTER(u, 7, RT_SHADOW_SET);

// glm's perspectiveRH_ZO with reverse Z: view-space depth (positive) from a 0..1 depth sample.
float RtShadowViewDistance(float depth)
{
    return ubo.proj[2][3] / (depth + ubo.proj[2][2]);
}

float3 RtShadowWorldPosition(int2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5) * rtShadow.invExtent;
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return world.xyz / world.w;
}

#endif
