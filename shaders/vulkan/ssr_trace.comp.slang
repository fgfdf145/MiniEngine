// Screen-space reflections, the trace. One ray a pixel: the reflection of the view about a GGX half
// vector drawn from the visible normals at the pixel's roughness, marched in screen space between
// its projected ends with view depth interpolated as 1/z, then refined by bisection. A hit takes its
// colour from last frame's anti-aliased image (TAA's history, before bloom) at the hit point
// reprojected into last frame, scaled into this frame's pre-exposure. Output: rgb radiance in HDR
// target units, a confidence (see ssr_common.slang for the fades). GT7 traces the same lobe with
// hardware rays; this is the screen-space version of that one term.

#include "d3d_register.slang"
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "ssr_common.slang"
#include "ssr_half_res.slang"

[[vk::binding(0, 1)]] Texture2D depthTexture : register(t0, space1);   // nearest
[[vk::binding(64, 1)]] SamplerState depthTextureSampler : register(s64, space1);
[[vk::binding(1, 1)]] Texture2D normalTexture : register(t1, space1);  // GB1, nearest
[[vk::binding(2, 1)]] Texture2D surfaceTexture : register(t2, space1); // GB2, nearest
[[vk::binding(3, 1)]] Texture2D historyTexture : register(t3, space1); // TAA history, last frame, linear
[[vk::binding(67, 1)]] SamplerState historyTextureSampler : register(s67, space1);
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ssrRawImage : register(u4, space1);
[[vk::binding(5, 1)]] Texture2D coatTexture : register(t5, space1);     // GB6, nearest
[[vk::binding(6, 1)]] Texture2D velocityTexture : register(t6, space1); // .ba: a mapped coat's normal, nearest

#include "ssr_lobe.slang"

// Must match SsrPushConstants in engine/renderer/vulkan/ssr_pass.cpp.
struct SsrConstants
{
    float2 extent;
    float2 invExtent;
    float maxDistance;
    float maxRoughness;
    // Current over previous pre-exposure: the history holds last frame's values.
    float historyScale;
    uint frameIndex;
    // SSR_TRACE_FLAG_* (ssr_half_res.slang).
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<SsrConstants> ssr D3D_PUSH_CONSTANTS(1);

static const int STEP_COUNT = 48;
static const int REFINE_COUNT = 6;
static const float THICKNESS_BASE = 0.1;
static const float THICKNESS_PER_METRE = 0.03;
// The last share of the ray over which a hit fades: it is about to run out of length.
static const float LENGTH_FADE_START = 0.8;

float ViewZFromDepth(float depth)
{
    return -ubo.proj[2][3] / (depth + ubo.proj[2][2]);
}

// Jimenez 2014, offset per frame so TAA and the resolve see a new sample every frame.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // Half resolution: this invocation traces one pixel of its 2x2 block (ssr_half_res.slang) and
    // writes its result at the block's place. At full resolution (ray reconstruction) its own pixel.
    bool fullResolution = (ssr.flags & SSR_TRACE_FLAG_FULL_RES) != 0u;
    int2 rawPixel = int2(dispatchThreadId.xy);
    if (any(rawPixel >= (fullResolution ? int2(ssr.extent) : SsrHalfResExtent(int2(ssr.extent)))))
    {
        return;
    }
    int2 pixel = fullResolution ? rawPixel : SsrHalfResSourcePixel(rawPixel, ssr.frameIndex, int2(ssr.extent));

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    // The coat's lobe on a coated pixel whose coat is the smoother (car paint), else the base's.
    SsrLobe lobe = ReadSsrLobe(pixel);
    float roughness = lobe.roughness;
    float roughnessFade = SsrRoughnessFade(roughness, ssr.maxRoughness);
    if (IsFarDepth(depth) || roughnessFade <= 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * ssr.invExtent;
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPosition = world.xyz / world.w;
    float3 N = lobe.normal;
    float3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);

    // A fresh sample every frame: the pixel's noise shifted by a per-frame offset.
    float2 frameOffset = float2(float(ssr.frameIndex % 64u) * 5.588238, float(ssr.frameIndex % 64u) * 7.123);
    float2 u = fullResolution ? SsrWhiteNoise(pixel, ssr.frameIndex)
                            : float2(InterleavedGradientNoise(float2(pixel) + frameOffset), InterleavedGradientNoise(float2(pixel.yx) + frameOffset * 1.37));
    float3 H = SampleGgxVisibleNormal(N, V, roughness, u);
    float3 R = reflect(-V, H);
    if (dot(R, N) <= 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    // The ray in view space, from just above the surface, clipped in front of the near plane.
    float3 viewStart = mul(ubo.view, float4(worldPosition + N * 0.01, 1.0)).xyz;
    float3 viewDirection = normalize(mul((float3x3)ubo.view, R));
    float rayLength = ssr.maxDistance;
    // The near plane sits at DEPTH_NEAR (negative view z).
    float nearZ = ViewZFromDepth(DEPTH_NEAR);
    if (viewStart.z + viewDirection.z * rayLength > nearZ)
    {
        rayLength = (nearZ * 1.01 - viewStart.z) / viewDirection.z;
    }
    float3 viewEnd = viewStart + viewDirection * rayLength;

    float4 clipStart = mul(ubo.proj, float4(viewStart, 1.0));
    float4 clipEnd = mul(ubo.proj, float4(viewEnd, 1.0));
    float kStart = 1.0 / clipStart.w;
    float kEnd = 1.0 / clipEnd.w;
    float2 uvStart = clipStart.xy * kStart * 0.5 + 0.5;
    float2 uvEnd = clipEnd.xy * kEnd * 0.5 + 0.5;
    // Interpolating z / w and 1 / w linearly in screen space gives the perspective-correct depth.
    float qStart = viewStart.z * kStart;
    float qEnd = viewEnd.z * kEnd;

    float jitter = InterleavedGradientNoise(float2(pixel) + frameOffset * 0.71);
    float previousT = 0.0;
    float hitT = -1.0;
    for (int step = 1; step <= STEP_COUNT; ++step)
    {
        float t = (float(step) - 1.0 + jitter) / float(STEP_COUNT);
        float2 sampleUv = lerp(uvStart, uvEnd, t);
        if (any(sampleUv < float2(0.0)) || any(sampleUv > float2(1.0)))
        {
            break;
        }
        float rayZ = lerp(qStart, qEnd, t) / lerp(kStart, kEnd, t);
        float sceneDepth = depthTexture.SampleLevel(depthTextureSampler, sampleUv, 0.0).r;
        if (!IsFarDepth(sceneDepth))
        {
            float sceneZ = ViewZFromDepth(sceneDepth);
            float behind = sceneZ - rayZ;
            if (behind > 0.0 && behind < THICKNESS_BASE + THICKNESS_PER_METRE * -sceneZ)
            {
                hitT = t;
                break;
            }
        }
        previousT = t;
    }

    if (hitT < 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    // Bisection between the last step in front and the first behind.
    float low = previousT;
    float high = hitT;
    for (int i = 0; i < REFINE_COUNT; ++i)
    {
        float middle = 0.5 * (low + high);
        float2 sampleUv = lerp(uvStart, uvEnd, middle);
        float rayZ = lerp(qStart, qEnd, middle) / lerp(kStart, kEnd, middle);
        float sceneZ = ViewZFromDepth(depthTexture.SampleLevel(depthTextureSampler, sampleUv, 0.0).r);
        if (sceneZ - rayZ > 0.0)
        {
            high = middle;
        }
        else
        {
            low = middle;
        }
    }
    float2 hitUv = lerp(uvStart, uvEnd, high);
    int2 hitPixel = clamp(int2(hitUv * ssr.extent), int2(0), int2(ssr.extent) - 1);
    float hitDepth = depthTexture.Load(int3(hitPixel, 0)).r;

    // A surface facing away from the ray shows its back, which the screen never rendered.
    float3 hitNormal = DecodeNormalOctahedral(normalTexture.Load(int3(hitPixel, 0)).rg);
    if (IsFarDepth(hitDepth) || dot(hitNormal, R) > 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    // Where the hit point was on last frame's screen.
    float4 hitWorld = mul(ubo.invViewProj, float4(hitUv * 2.0 - 1.0, hitDepth, 1.0));
    float4 previousClip = mul(ubo.prevViewProj, float4(hitWorld.xyz / hitWorld.w, 1.0));
    float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
    if (previousClip.w <= 0.0 || any(previousUv < float2(0.0)) || any(previousUv > float2(1.0)))
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    float3 radiance = historyTexture.SampleLevel(historyTextureSampler, previousUv, 0.0).rgb * ssr.historyScale;
    if (any(isnan(radiance)) || any(isinf(radiance)))
    {
        radiance = float3(0.0);
    }
    float confidence = roughnessFade * SsrEdgeFade(hitUv) * SsrEdgeFade(previousUv) *
                       (1.0 - smoothstep(LENGTH_FADE_START, 1.0, high));
    ssrRawImage[rawPixel] = float4(min(radiance, float3(65504.0)), confidence);
}
