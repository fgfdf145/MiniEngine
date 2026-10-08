// Depth-aware 5x5 spatial filter of the raw trace, then exponential temporal
// accumulation reprojected through the motion vectors. Two values go through it together: r the AO,
// g the DDGI probe occlusion the ray traced pass adds (1 without it). History holds (ao, view
// distance, sample count, probe occlusion); a history sample whose distance disagrees with where this
// surface was last frame is a disocclusion and is dropped.

#include "scene_common.slang"
#include "vbao_common.slang"

[[vk::binding(0, 1)]] Sampler2D aoRawTexture;
[[vk::binding(1, 1)]] Sampler2D depthTexture;
[[vk::binding(2, 1)]] Sampler2D velocityTexture;
[[vk::binding(3, 1)]] Sampler2D historyTexture;
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyImage;
[[vk::binding(5, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> aoImage;

static const float MAX_HISTORY = 16.0;
static const float SPATIAL_DEPTH_TOLERANCE = 0.05;
static const float TEMPORAL_DEPTH_TOLERANCE = 0.05;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    int2 extent = int2(aoConstants.extent);
    if (any(pixel >= extent))
    {
        return;
    }

    // AO and the probe occlusion off: the lighting pass and the debug view read a neutral 1.0, and
    // history is left alone (the renderer reports it invalid for the next frame).
    if ((aoConstants.flags & (AO_FLAG_ENABLED | AO_FLAG_PROBE_OCCLUSION)) == 0u)
    {
        aoImage[pixel] = float4(1.0);
        return;
    }

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        historyImage[pixel] = float4(1.0, 0.0, 0.0, 1.0);
        aoImage[pixel] = float4(1.0);
        return;
    }
    float distanceToCamera = -ViewZFromDepth(depth);

    // The trace ran at half resolution (HalfResSourcePixel): upsample it with a depth-aware filter
    // over the 3x3 half-resolution samples around this pixel, about the 5x5 full-resolution
    // neighbourhood the spatial filter covered at full resolution. Without the spatial filter only
    // the samples within a pixel or so count.
    int2 halfExtent = HalfResExtent();
    int2 center = min(pixel / 2, halfExtent - 1);
    bool spatial = (aoConstants.flags & AO_FLAG_SPATIAL) != 0u;
    float2 current = aoRawTexture.Load(int3(center, 0)).rg;
    float2 sum = float2(0.0);
    float weightSum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 sampleCoord = clamp(center + int2(x, y), int2(0), halfExtent - 1);
            int2 source = HalfResSourcePixel(sampleCoord);
            // A tent over the full-resolution distance to where the sample was traced, so the
            // weights change across each 2x2 block instead of repeating over it.
            float2 offset = abs(float2(source - pixel));
            float2 tent = max(1.0 - offset / (spatial ? 3.5 : 1.5), 0.0);
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
            float weight = distanceWeight * max(0.0, 1.0 - abs(neighborDistance - distanceToCamera) / (SPATIAL_DEPTH_TOLERANCE * distanceToCamera));
            sum += weight * aoRawTexture.Load(int3(sampleCoord, 0)).rg;
            weightSum += weight;
        }
    }
    current = weightSum > 0.0 ? sum / weightSum : current;

    float2 result = current;
    float sampleCount = 1.0;
    bool temporal = (aoConstants.flags & AO_FLAG_TEMPORAL) != 0u;
    bool historyValid = (aoConstants.flags & AO_FLAG_HISTORY_VALID) != 0u;
    if (temporal && historyValid)
    {
        float2 uv = (float2(pixel) + 0.5) * aoConstants.invExtent;
        float2 previousUv = uv - velocityTexture.Load(int3(pixel, 0)).rg;
        if (all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0)))
        {
            float4 history = historyTexture.SampleLevel(previousUv, 0.0);
            // Where this surface point sat last frame, assuming it did not move: exact for static
            // geometry, and close for slow movers.
            float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
            float expectedDistance = mul(ubo.prevViewProj, float4(world.xyz / world.w, 1.0)).w;
            if (abs(history.g - expectedDistance) < TEMPORAL_DEPTH_TOLERANCE * expectedDistance)
            {
                sampleCount = min(history.b + 1.0, MAX_HISTORY);
                result = lerp(history.ra, current, 1.0 / sampleCount);
            }
        }
    }

    // Without AO the trace wrote r = 1; the probe occlusion alone may be on.
    if ((aoConstants.flags & AO_FLAG_ENABLED) == 0u)
    {
        result.x = 1.0;
    }
    if ((aoConstants.flags & AO_FLAG_PROBE_OCCLUSION) == 0u)
    {
        result.y = 1.0;
    }
    historyImage[pixel] = float4(result.x, distanceToCamera, sampleCount, result.y);
    aoImage[pixel] = float4(result, 0.0, 0.0);
}
