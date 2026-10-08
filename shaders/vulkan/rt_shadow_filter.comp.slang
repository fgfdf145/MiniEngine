// The ray traced sun shadow's spatial filter (VulkanRtShadowPass), into SceneShadow: the accumulated
// visibility blurred over the penumbra the sun's disk casts. A penumbra is the occluder's distance
// times the sun's angular diameter wide; the filter spreads its 5 x 5 taps over that many pixels
// (at least one, so contact shadows stay sharp) and weights them by depth and normal, so the blur
// never crosses onto another surface. Few samples accumulated so far widen it a little. With the
// denoiser off it passes the raw trace through; while nothing traces it writes (1, 0).

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "rt_shadow_common.slang"

static const float DEPTH_TOLERANCE = 0.03;
static const float MAX_RADIUS_PIXELS = 24.0;

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
    if ((rtShadow.flags & RT_SHADOW_FLAG_ENABLED) == 0u)
    {
        rtShadowOutput[pixel] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    float depth = rtShadowDepth.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        rtShadowOutput[pixel] = float4(1.0, 1.0, 0.0, 0.0);
        return;
    }
    if ((rtShadow.flags & RT_SHADOW_FLAG_DENOISE) == 0u)
    {
        rtShadowOutput[pixel] = float4(rtShadowRaw[pixel].x, 1.0, 0.0, 0.0);
        return;
    }

    float4 center = rtShadowAccumulated[pixel];
    // Every ray of a settled history agreed: fully lit or fully shadowed, nothing to blur (a blur would
    // only pull a neighbouring penumbra over it). Most of the screen ends here.
    if (center.z >= 4.0 && (center.x <= 0.001 || center.x >= 0.999))
    {
        rtShadowOutput[pixel] = float4(center.x, 1.0, 0.0, 0.0);
        return;
    }
    float distanceToCamera = RtShadowViewDistance(depth);
    float3 normal = DecodeNormalOctahedral(rtShadowNormal.Load(int3(pixel, 0)).ba);

    // The penumbra's width in pixels: the occluder's distance times the sun's angular diameter, over
    // one pixel's footprint at this depth.
    float cosRadius = clamp(ubo.sunIlluminance.w, 0.99, 1.0);
    float tanRadius = sqrt(max(1.0 - cosRadius * cosRadius, 0.0)) / cosRadius;
    float pixelWorld = 2.0 * distanceToCamera / (abs(ubo.proj[1][1]) * rtShadow.extent.y);
    float penumbraPixels = 2.0 * center.w * tanRadius / max(pixelWorld, 1e-6);
    // Fewer samples than a converged history: wider, to hide their noise.
    float youth = clamp(1.0 - center.z / 8.0, 0.0, 1.0);
    float radius = clamp(penumbraPixels + 2.0 * youth, 1.0, MAX_RADIUS_PIXELS);
    float stepPixels = max(1.0, floor(radius / 2.0));
    float sigma = max(radius * 0.5, 0.5);

    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            float2 offset = float2(x, y) * stepPixels;
            int2 neighbour = pixel + int2(offset);
            if (any(neighbour < int2(0)) || any(neighbour >= extent))
            {
                continue;
            }
            float neighbourDepth = rtShadowDepth.Load(int3(neighbour, 0)).r;
            if (IsFarDepth(neighbourDepth))
            {
                continue;
            }
            float neighbourDistance = RtShadowViewDistance(neighbourDepth);
            float depthWeight = max(0.0, 1.0 - abs(neighbourDistance - distanceToCamera) / (DEPTH_TOLERANCE * distanceToCamera * (1.0 + 0.25 * length(float2(x, y)) * stepPixels)));
            float3 neighbourNormal = DecodeNormalOctahedral(rtShadowNormal.Load(int3(neighbour, 0)).ba);
            float normalWeight = pow(max(dot(normal, neighbourNormal), 0.0), 16.0);
            float spatialWeight = exp(-dot(offset, offset) / (2.0 * sigma * sigma));
            float weight = spatialWeight * depthWeight * normalWeight;
            sum += weight * rtShadowAccumulated[neighbour].x;
            weightSum += weight;
        }
    }
    float visibility = weightSum > 0.0 ? sum / weightSum : center.x;
    rtShadowOutput[pixel] = float4(visibility, 1.0, 0.0, 0.0);
}
