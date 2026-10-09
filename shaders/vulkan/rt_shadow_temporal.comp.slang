// The ray traced sun shadow's temporal filter (VulkanRtShadowPass): this frame's one-ray visibility
// averaged into the history reprojected through the motion vectors, as the AO resolve does. The
// history is first clamped to the range of the raw visibility around the pixel: where every ray
// nearby agrees (fully lit or fully shadowed), the history must too, so a shadow that moves (a car's
// on the road) leaves no trail, while in a penumbra, where the raw rays disagree, it accumulates
// freely. A history the clamp had to move restarts the average from a few samples.

#include "scene_common.slang"
#include "rt_shadow_common.slang"

static const float MAX_SAMPLES = 24.0;
static const float TEMPORAL_DEPTH_TOLERANCE = 0.05;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    int2 extent = int2(rtShadow.extent);
    if (any(pixel >= extent))
    {
        return;
    }
    float depth = rtShadowDepth.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        rtShadowHistoryWrite[pixel] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    float2 raw = rtShadowRaw[pixel].xy;
    float lowest = raw.x;
    float highest = raw.x;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 neighbour = clamp(pixel + int2(x, y), int2(0), extent - 1);
            if (IsFarDepth(rtShadowDepth.Load(int3(neighbour, 0)).r))
            {
                continue;
            }
            float value = rtShadowRaw[neighbour].x;
            lowest = min(lowest, value);
            highest = max(highest, value);
        }
    }

    float distanceToCamera = RtShadowViewDistance(depth);
    float visibility = raw.x;
    float occluder = raw.y;
    float samples = 1.0;
    if ((rtShadow.flags & RT_SHADOW_FLAG_HISTORY_VALID) != 0u)
    {
        float2 uv = (float2(pixel) + 0.5) * rtShadow.invExtent;
        float2 previousUv = uv - rtShadowVelocity.Load(int3(pixel, 0)).rg;
        if (all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0)))
        {
            float4 history = rtShadowHistoryRead.SampleLevel(rtShadowHistoryReadSampler, previousUv, 0.0);
            float3 world = RtShadowWorldPosition(pixel, depth);
            float expectedDistance = mul(ubo.prevViewProj, float4(world, 1.0)).w;
            if (abs(history.y - expectedDistance) < TEMPORAL_DEPTH_TOLERANCE * expectedDistance)
            {
                float clamped = clamp(history.x, lowest, highest);
                samples = abs(clamped - history.x) > 0.1 ? 2.0 : min(history.z + 1.0, MAX_SAMPLES);
                visibility = lerp(clamped, raw.x, 1.0 / samples);
                // The occluders' distance, from the rays that found one.
                occluder = raw.y > 0.0 ? (history.w > 0.0 ? lerp(history.w, raw.y, 0.25) : raw.y) : history.w;
            }
        }
    }
    rtShadowHistoryWrite[pixel] = float4(visibility, distanceToCamera, samples, occluder);
}
