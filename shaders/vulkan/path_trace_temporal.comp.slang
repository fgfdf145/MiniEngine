// The path tracer's temporal accumulation (VulkanPathTracePass): last frame's average, reprojected
// through the motion vectors, takes this frame's paths in as one more sample, up to the frame's
// history cap; a still image's cap grows every frame, so it converges to the reference. The history is
// read with a bilinear footprint whose taps are each kept only where last frame's surface (its view
// depth and normal, stored beside it) is the one this pixel sees, so a disocclusion starts afresh
// rather than smearing what was in front. The specular channel's history is shortened by its
// roughness while anything moves: a sharp reflection does not move with the surface.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "path_trace_common.slang"

// How far last frame's view depth may lie from where this surface was, as a share of it, and how
// closely its normal must agree.
static const float DEPTH_TOLERANCE = 0.03;
static const float NORMAL_TOLERANCE = 0.85;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(pathTrace.extent)))
    {
        return;
    }
    float depth = ptDepth.Load(int3(PtGBufferPixel(pixel), 0)).r;
    float4 rawDiffuse = ptRawDiffuse[pixel];
    float4 rawSpecular = ptRawSpecular[pixel];
    if (!PathTraceSurfaceShaded(pixel, depth))
    {
        ptHistoryDiffuse[pixel] = float4(0.0);
        ptHistorySpecular[pixel] = float4(0.0);
        ptHistorySurface[pixel] = float4(0.0);
        return;
    }

    float3 worldPosition = PathTraceWorldPosition(pixel, depth);
    float3 N = DecodeNormalOctahedral(ptNormal.Load(int3(PtGBufferPixel(pixel), 0)).rg);
    float viewDepth = mul(mul(ubo.proj, ubo.view), float4(worldPosition, 1.0)).w;
    ptHistorySurface[pixel] = float4(viewDepth, EncodeNormalOctahedral(N), 0.0);

    float4 previousDiffuse = float4(0.0);
    float4 previousSpecular = float4(0.0);
    float previousWeight = 0.0;
    if ((pathTrace.flags & PT_FLAG_HISTORY_VALID) != 0u)
    {
        // Where this surface was last frame, and how far from the camera.
        float2 uv = (float2(pixel) + 0.5) * pathTrace.invExtent;
        float2 previousUv = uv - ptVelocity.Load(int3(PtGBufferPixel(pixel), 0)).rg;
        float previousViewDepth = mul(ubo.prevViewProj, float4(worldPosition, 1.0)).w;
        float2 position = previousUv * pathTrace.extent - 0.5;
        int2 base = int2(floor(position));
        float2 f = position - float2(base);
        for (int tap = 0; tap < 4; ++tap)
        {
            int2 offset = int2(tap & 1, tap >> 1);
            int2 source = base + offset;
            if (any(source < int2(0)) || any(source >= int2(pathTrace.extent)))
            {
                continue;
            }
            float4 surface = ptPreviousSurface.Load(int3(source, 0));
            if (surface.x <= 0.0 || abs(surface.x - previousViewDepth) > DEPTH_TOLERANCE * previousViewDepth ||
                dot(DecodeNormalOctahedral(surface.yz), N) < NORMAL_TOLERANCE)
            {
                continue;
            }
            float2 bilinear = lerp(float2(1.0) - f, f, float2(offset));
            float weight = bilinear.x * bilinear.y;
            previousDiffuse += ptPreviousDiffuse.Load(int3(source, 0)) * weight;
            previousSpecular += ptPreviousSpecular.Load(int3(source, 0)) * weight;
            previousWeight += weight;
        }
    }

    float diffuseCount = 1.0;
    float specularCount = 1.0;
    float3 diffuse = rawDiffuse.rgb;
    float3 specular = rawSpecular.rgb;
    if (previousWeight > 1e-3)
    {
        previousDiffuse /= previousWeight;
        previousSpecular /= previousWeight;
        // While the image changes, a glossy reflection keeps a history as short as its lobe is sharp.
        float specularCap = pathTrace.historyCap;
        if (pathTrace.historyCap <= pathTrace.motionFrames)
        {
            specularCap = max(2.0, pathTrace.motionFrames * smoothstep(0.0, 0.5, PathTraceSpecularRoughness(pixel)));
        }
        diffuseCount = min(previousDiffuse.a + 1.0, pathTrace.historyCap);
        specularCount = min(previousSpecular.a + 1.0, specularCap);
        diffuse = lerp(previousDiffuse.rgb * pathTrace.historyScale, rawDiffuse.rgb, 1.0 / diffuseCount);
        specular = lerp(previousSpecular.rgb * pathTrace.historyScale, rawSpecular.rgb, 1.0 / specularCount);
    }
    ptHistoryDiffuse[pixel] = float4(min(diffuse, float3(65504.0)), diffuseCount);
    ptHistorySpecular[pixel] = float4(min(specular, float3(65504.0)), specularCount);
}
