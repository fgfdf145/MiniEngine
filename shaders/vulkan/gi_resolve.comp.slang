// Filters the one-bounce trace (gi_trace.comp) into SceneGi: a depth- and normal-aware 5x5 spatial
// filter, then exponential temporal accumulation reprojected through the motion vectors, as
// vbao_resolve.comp filters the AO. History holds (rgb, packed distance and sample count); a history
// sample whose distance disagrees with where this surface was last frame is a disocclusion and is
// dropped. The history is read with nearest filtering: the packed channel does not interpolate.

#include "shader_helpers.slang"
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "vbao_common.slang"

[[vk::binding(0, 1)]] Sampler2D giRawTexture;
[[vk::binding(1, 1)]] Sampler2D depthTexture;
[[vk::binding(2, 1)]] Sampler2D velocityTexture;
[[vk::binding(3, 1)]] Sampler2D normalTexture;
[[vk::binding(4, 1)]] Sampler2D historyTexture;
[[vk::binding(5, 1)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> historyImage;
[[vk::binding(6, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> giImage;

// More than the AO's 16: the bounce is noisier (a few samples carry bright light) and changes slowly.
static const float MAX_HISTORY = 32.0;
static const float SPATIAL_DEPTH_TOLERANCE = 0.05;
static const float SPATIAL_NORMAL_POWER = 8.0;
static const float TEMPORAL_DEPTH_TOLERANCE = 0.05;
// The history's w: the view distance in centimetres (to 2 km) times 64, plus the sample count.
static const float COUNT_RANGE = 64.0;
static const float MAX_PACKED_DISTANCE = 2000.0;

float PackDistanceAndCount(float distanceToCamera, float count)
{
    return floor(min(distanceToCamera, MAX_PACKED_DISTANCE) * 100.0) * COUNT_RANGE + count;
}

void UnpackDistanceAndCount(float packed, out float distanceToCamera, out float count)
{
    count = GlslMod(packed, COUNT_RANGE);
    distanceToCamera = floor(packed / COUNT_RANGE) / 100.0;
}

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

    // Off: the composite and the debug view read no light.
    if ((aoConstants.flags & AO_FLAG_ENABLED) == 0u)
    {
        giImage[pixel] = float4(0.0);
        return;
    }

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        historyImage[pixel] = float4(0.0);
        giImage[pixel] = float4(0.0);
        return;
    }
    float distanceToCamera = -ViewZFromDepth(depth);
    float3 normal = DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).ba);

    // The trace ran at half resolution (HalfResSourcePixel): upsample it with a depth- and
    // normal-aware filter over the 3x3 half-resolution samples around this pixel, about the 5x5
    // full-resolution neighbourhood the spatial filter covered at full resolution. Without the
    // spatial filter only the samples within a pixel or so count.
    int2 halfExtent = HalfResExtent();
    int2 center = min(pixel / 2, halfExtent - 1);
    bool spatial = (aoConstants.flags & AO_FLAG_SPATIAL) != 0u;
    float3 current = giRawTexture.Load(int3(center, 0)).rgb;
    float3 sum = float3(0.0);
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
            float depthWeight = max(0.0, 1.0 - abs(neighborDistance - distanceToCamera) / (SPATIAL_DEPTH_TOLERANCE * distanceToCamera));
            float3 neighborNormal = DecodeNormalOctahedral(normalTexture.Load(int3(source, 0)).ba);
            float normalWeight = pow(max(dot(normal, neighborNormal), 0.0), SPATIAL_NORMAL_POWER);
            float weight = distanceWeight * depthWeight * normalWeight;
            sum += weight * giRawTexture.Load(int3(sampleCoord, 0)).rgb;
            weightSum += weight;
        }
    }
    current = weightSum > 0.0 ? sum / weightSum : current;

    float3 result = current;
    float sampleCount = 1.0;
    bool temporal = (aoConstants.flags & AO_FLAG_TEMPORAL) != 0u;
    bool historyValid = (aoConstants.flags & AO_FLAG_HISTORY_VALID) != 0u;
    if (temporal && historyValid)
    {
        float2 uv = (float2(pixel) + 0.5) * aoConstants.invExtent;
        float2 previousUv = uv - velocityTexture.Load(int3(pixel, 0)).rg;
        if (all(previousUv >= float2(0.0)) && all(previousUv < float2(1.0)))
        {
            float4 history = historyTexture.Load(int3(int2(previousUv * aoConstants.extent), 0));
            float historyDistance;
            float historyCount;
            UnpackDistanceAndCount(history.w, historyDistance, historyCount);
            float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
            float expectedDistance = mul(ubo.prevViewProj, float4(world.xyz / world.w, 1.0)).w;
            if (historyCount >= 1.0 && abs(historyDistance - expectedDistance) < TEMPORAL_DEPTH_TOLERANCE * expectedDistance)
            {
                sampleCount = min(historyCount + 1.0, MAX_HISTORY);
                result = lerp(history.rgb, current, 1.0 / sampleCount);
            }
        }
    }

    historyImage[pixel] = float4(result, PackDistanceAndCount(distanceToCamera, sampleCount));
    giImage[pixel] = float4(result, 1.0);
}
