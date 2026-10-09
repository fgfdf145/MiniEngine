// Occlusion traced through the ray scene with hardware ray queries (docs/design/
// 2026-10-07-ray-traced-effects-design.md), at half resolution like the visibility bitmask AO it
// replaces or joins (VulkanAoTracePass). A few cosine-distributed rays leave each pixel's surface:
//   r: the share that travels the AO radius without a hit (with AO_FLAG_RAY_TRACED_AO; otherwise the
//      bitmask's value, which this pass reads back and keeps);
//   g: the DDGI probe occlusion, where a level coarser than the finest answers the pixel's lookup:
//      of the rays that cleared the AO radius, the share that also clears that level's spacing. The
//      lighting scales the probes' light by it, so an alcove smaller than the spacing is not lit as
//      if open (the DDGI design's "Occlusion Finer Than the Probes"). The near-field occluders are
//      the AO's, so the two multiply without counting any twice.
// Alpha-tested surfaces are tested against their textures (RAY_TEXTURED_ALPHA), and moving instances
// occlude too.

#define RAY_TEXTURED_ALPHA
#include "scene_common.slang"
#include "gbuffer_common.slang"
#define AO_PASS_SET 2
#include "vbao_common.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"
#include "ddgi_common.slang"

[[vk::binding(0, 2)]] Texture2D depthTexture : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D normalTexture : register(t1, space2);
[[vk::binding(2, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> aoRawImage : register(u2, space2);

static const float PI = 3.14159265359;

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// A cosine-distributed direction about n for (u1, u2) in [0, 1)^2.
float3 CosineDirection(float3 n, float u1, float u2)
{
    float3 helper = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, n));
    float3 bitangent = cross(n, tangent);
    float radius = sqrt(u1);
    float phi = 2.0 * PI * u2;
    return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
}

// How much of a surface's DDGI lookup a level coarser than the finest answers, and that level's
// spacing (weighted over the levels that share it): DdgiIrradianceAlong's fade, without the probes'
// coverage, which only the lookup itself knows.
float2 CoarseProbeShare(float3 position)
{
    float remaining = 1.0;
    float coarseWeight = 0.0;
    float coarseSpacing = 0.0;
    uint levelCount = DdgiLevelCount();
    for (uint level = 0u; level < levelCount && remaining > 1e-3; ++level)
    {
        float spacing = DdgiLevelSpacing(level);
        float3 fromCamera = abs(position - ubo.cameraWorldPosition.xyz) / spacing;
        float3 toEdge = (float3(DDGI_GRID / 2 - 1) - 0.5 - fromCamera) / DDGI_FADE_CELLS;
        float fade = clamp(min(min(toEdge.x, toEdge.y), toEdge.z), 0.0, 1.0);
        float weight = fade * remaining;
        if (level > 0u)
        {
            coarseWeight += weight;
            coarseSpacing += weight * spacing;
        }
        remaining *= 1.0 - fade;
    }
    return float2(coarseWeight, coarseWeight > 0.0 ? coarseSpacing / coarseWeight : 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 halfPixel = int2(dispatchThreadId.xy);
    if (any(halfPixel >= HalfResExtent()))
    {
        return;
    }
    int2 pixel = HalfResSourcePixel(halfPixel);
    bool tracedAo = (aoConstants.flags & AO_FLAG_RAY_TRACED_AO) != 0u;
    bool aoEnabled = (aoConstants.flags & AO_FLAG_ENABLED) != 0u;
    bool probeOcclusion = (aoConstants.flags & AO_FLAG_PROBE_OCCLUSION) != 0u;
    // The bitmask ran first where it still gives the AO.
    float ao = aoEnabled && !tracedAo ? aoRawImage[halfPixel].r : 1.0;

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (IsFarDepth(depth))
    {
        aoRawImage[halfPixel] = float4(1.0);
        return;
    }
    float2 uv = (float2(pixel) + 0.5) * aoConstants.invExtent;
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 position = world.xyz / world.w;
    float3 normal = DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).ba);

    // Rays shorter than the AO radius find what the AO counts; the probe occlusion counts what lies
    // between it and the coarse level's spacing.
    float nearLimit = aoEnabled ? aoConstants.radius : 0.0;
    float2 coarse = probeOcclusion ? CoarseProbeShare(position) : float2(0.0);
    float farLimit = coarse.x > 0.0 ? coarse.y : 0.0;
    float tMax = max(tracedAo ? nearLimit : 0.0, farLimit);
    if (tMax <= 0.0 || (!tracedAo && farLimit <= nearLimit))
    {
        aoRawImage[halfPixel] = float4(ao, 1.0, 0.0, 0.0);
        return;
    }

    // Off the surface by more than the depth buffer's reconstruction error, which grows with distance.
    float distanceToCamera = length(position - ubo.cameraWorldPosition.xyz);
    float3 origin = OffsetRayOrigin(position + normal * (0.002 + 0.0005 * distanceToCamera), normal);

    uint rayCount = max(aoConstants.sliceCount, 1u);
    float noise = InterleavedGradientNoise(float2(pixel) + 5.588238 * float(aoConstants.frameIndex % 64u));
    uint pixelId = uint(pixel.x) + uint(pixel.y) * 65536u;
    uint nearHits = 0u;
    uint escaped = 0u;
    for (uint ray = 0u; ray < rayCount; ++ray)
    {
        // Stratified around the circle by the noise, a hash over the radius.
        float u2 = frac(noise + (float(ray) + 0.5) / float(rayCount));
        float u1 = RayHash(pixelId, aoConstants.frameIndex * 16u + ray);
        float3 direction = CosineDirection(normal, u1, u2);
        RayHit hit;
        uint rayId = pixelId * 16u + ray + aoConstants.frameIndex * 7919u;
        bool found = TraceSceneRayMasked(origin, direction, 0.0, tMax, false, rayId, RAY_MASK_VISIBILITY, hit);
        if (found && hit.t < nearLimit)
        {
            ++nearHits;
        }
        else if (!found || hit.t >= farLimit)
        {
            ++escaped;
        }
    }
    if (tracedAo)
    {
        ao = 1.0 - float(nearHits) / float(rayCount);
    }
    float probeVisibility = 1.0;
    if (farLimit > nearLimit && nearHits < rayCount)
    {
        probeVisibility = lerp(1.0, float(escaped) / float(rayCount - nearHits), coarse.x);
    }
    aoRawImage[halfPixel] = float4(ao, probeVisibility, 0.0, 0.0);
}
