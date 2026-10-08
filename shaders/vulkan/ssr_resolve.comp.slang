// Screen-space reflections, the resolve: GT7 reconstructs its one-ray-a-pixel trace with sample
// reuse, a temporal filter and a joint bilateral filter; this does the last two. A spatial pass
// over the raw trace, depth- and normal-aware, whose footprint grows with roughness (a mirror keeps
// its own sample), then exponential accumulation of history reprojected through the surface's
// motion vector and clipped to the current neighbourhood, so a reflection that moved or appeared
// does not trail. Values are premultiplied by confidence while they are averaged.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "ssr_half_res.slang"

[[vk::binding(0, 1)]] Sampler2D ssrRawTexture;  // nearest
[[vk::binding(1, 1)]] Sampler2D depthTexture;   // nearest
[[vk::binding(2, 1)]] Sampler2D normalTexture;  // GB1, nearest
[[vk::binding(3, 1)]] Sampler2D surfaceTexture; // GB2, nearest
[[vk::binding(4, 1)]] Sampler2D velocityTexture;
[[vk::binding(5, 1)]] Sampler2D historyTexture; // linear
[[vk::binding(6, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyImage;
[[vk::binding(7, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> reflectionsImage;
[[vk::binding(8, 1)]] Sampler2D coatTexture;       // GB6, nearest

#include "ssr_lobe.slang"

// Must match SsrResolvePushConstants in engine/renderer/vulkan/ssr_pass.cpp.
struct SsrResolveConstants
{
    float2 extent;
    float2 invExtent;
    // The history holds last frame's pre-exposed values; this brings them to this frame's.
    float historyScale;
    uint flags;
    // The trace's frame index, which says which pixel of each 2x2 block it traced.
    uint frameIndex;
    float unused;
};
[[vk::push_constant]] ConstantBuffer<SsrResolveConstants> resolve;

static const uint SSR_FLAG_TRACED = 1u;
static const uint SSR_FLAG_HISTORY_VALID = 2u;
// The trace ran at full resolution for DLSS ray reconstruction (ssr_half_res.slang): each pixel's raw
// sample goes to the lighting pass as it is, and the denoiser filters it.
static const uint SSR_FLAG_PASS_THROUGH = 4u;

static const float CURRENT_WEIGHT = 0.125;
static const float DEPTH_TOLERANCE = 0.05;
static const float NORMAL_POWER = 16.0;
// Roughness at which the spatial footprint reaches its full width.
static const float FULL_FOOTPRINT_ROUGHNESS = 0.3;
// The footprint's tent over the full-resolution distance to where a half-resolution sample was
// traced: a mirror interpolates the samples within a pixel or so, a rough lobe averages a 3.5-pixel
// radius, about the 5x5 of the full-resolution trace.
static const float MIRROR_TENT_RADIUS = 1.5;
static const float ROUGH_TENT_RADIUS = 3.5;
// How many standard deviations of the neighbourhood the history may stray from its mean.
static const float CLIP_DEVIATIONS = 1.5;

float ViewZFromDepth(float depth)
{
    return -ubo.proj[2][3] / (depth + ubo.proj[2][2]);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    int2 extent = int2(resolve.extent);
    if (any(pixel >= extent))
    {
        return;
    }

    // Nothing traced (SSR off, or no history to take colour from): the lighting pass sees zero
    // confidence and uses the environment. History is left alone; the renderer invalidates it.
    float depth = depthTexture.Load(int3(pixel, 0)).r;
    if ((resolve.flags & SSR_FLAG_TRACED) == 0u || IsFarDepth(depth))
    {
        reflectionsImage[pixel] = float4(0.0);
        if ((resolve.flags & SSR_FLAG_TRACED) != 0u)
        {
            historyImage[pixel] = float4(0.0);
        }
        return;
    }

    if ((resolve.flags & SSR_FLAG_PASS_THROUGH) != 0u)
    {
        float4 raw = ssrRawTexture.Load(int3(pixel, 0));
        if (any(isnan(raw)) || any(isinf(raw)))
        {
            raw = float4(0.0);
        }
        // Kept as history too (premultiplied, as the filtered path stores it), so leaving ray
        // reconstruction starts from it.
        historyImage[pixel] = float4(raw.rgb * raw.a, raw.a);
        reflectionsImage[pixel] = float4(min(raw.rgb, float3(65504.0)), clamp(raw.a, 0.0, 1.0));
        return;
    }

    float distanceToCamera = -ViewZFromDepth(depth);
    // The lobe the trace followed (ssr_lobe.slang): the coat's on car paint.
    SsrLobe lobe = ReadSsrLobe(pixel);
    float3 N = lobe.normal;
    float roughness = lobe.roughness;
    float tentRadius = lerp(MIRROR_TENT_RADIUS, ROUGH_TENT_RADIUS, clamp(roughness / FULL_FOOTPRINT_ROUGHNESS, 0.0, 1.0));

    // The trace ran at half resolution (ssr_half_res.slang): gather the 3x3 half-resolution samples
    // around this pixel, weighted by the depth and normal of the pixel each was traced at and by a
    // tent over the distance to it, premultiplied by confidence. All nine bound the history's
    // clipping through their mean and spread (variance clipping): the min and max of the few that
    // lie close are too narrow a box at one sample per block, and pulled the history down.
    int2 halfExtent = SsrHalfResExtent(extent);
    int2 center = min(pixel / 2, halfExtent - 1);
    float4 sum = float4(0.0);
    float weightSum = 0.0;
    float4 moment1 = float4(0.0);
    float4 moment2 = float4(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 sampleCoord = clamp(center + int2(x, y), int2(0), halfExtent - 1);
            int2 source = SsrHalfResSourcePixel(sampleCoord, resolve.frameIndex, extent);
            float2 offset = abs(float2(source - pixel));
            float4 raw = ssrRawTexture.Load(int3(sampleCoord, 0));
            float4 premultiplied = float4(raw.rgb * raw.a, raw.a);
            moment1 += premultiplied;
            moment2 += premultiplied * premultiplied;
            float2 tent = max(1.0 - offset / tentRadius, 0.0);
            float distanceWeight = tent.x * tent.y;
            if (distanceWeight <= 0.0)
            {
                continue;
            }
            float neighborDepth = depthTexture.Load(int3(source, 0)).r;
            if (IsFarDepth(neighborDepth))
            {
                continue;
            }
            float neighborDistance = -ViewZFromDepth(neighborDepth);
            float3 neighborNormal = ReadSsrLobe(source).normal;
            float weight = distanceWeight *
                           max(0.0, 1.0 - abs(neighborDistance - distanceToCamera) / (DEPTH_TOLERANCE * distanceToCamera)) *
                           pow(max(dot(N, neighborNormal), 0.0), NORMAL_POWER);
            sum += weight * premultiplied;
            weightSum += weight;
        }
    }
    float4 current = weightSum > 0.0 ? sum / weightSum : float4(0.0);
    float4 mean = moment1 / 9.0;
    float4 deviation = sqrt(max(moment2 / 9.0 - mean * mean, float4(0.0)));
    float4 neighbourhoodMin = max(mean - CLIP_DEVIATIONS * deviation, float4(0.0));
    float4 neighbourhoodMax = mean + CLIP_DEVIATIONS * deviation;

    float4 result = current;
    if ((resolve.flags & SSR_FLAG_HISTORY_VALID) != 0u)
    {
        float2 uv = (float2(pixel) + 0.5) * resolve.invExtent;
        float2 previousUv = uv - velocityTexture.Load(int3(pixel, 0)).rg;
        if (all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0)))
        {
            float4 history = historyTexture.SampleLevel(previousUv, 0.0);
            history.rgb *= resolve.historyScale;
            history = clamp(history, neighbourhoodMin, neighbourhoodMax);
            result = lerp(history, current, CURRENT_WEIGHT);
        }
    }
    if (any(isnan(result)) || any(isinf(result)))
    {
        result = float4(0.0);
    }

    historyImage[pixel] = result;
    // Un-premultiplied for the lighting pass: radiance, and how far to trust it.
    float3 radiance = result.a > 1e-4 ? result.rgb / result.a : float3(0.0);
    reflectionsImage[pixel] = float4(min(radiance, float3(65504.0)), clamp(result.a, 0.0, 1.0));
}
