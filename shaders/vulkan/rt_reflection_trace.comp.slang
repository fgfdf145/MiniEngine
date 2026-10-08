// Ray traced reflections (VulkanSsrTracePass's hardware variant, docs/design/
// 2026-10-07-ray-traced-effects-design.md): the screen-space trace's lobe and output, with the ray
// traced through the scene instead of marched against the depth buffer, so what is off screen or
// hidden is reflected too. One ray a pixel at half resolution (every pixel, with white noise, under
// DLSS ray reconstruction), drawn from the GGX visible normals of the pixel's lobe (the coat's on car
// paint, ssr_lobe.slang). Where the hit is the surface the screen shows at that point, its colour
// comes from last frame's anti-aliased image as the screen-space trace
// takes it; elsewhere the hit is shaded here: its material from the mesh and the material's textures
// (ray_hit_common.slang), lit by ShadeSurface with every light, the DDGI probes' ambient and a traced
// shadow ray toward the sun. A miss sees the sky. Writes SsrRaw for the SSR resolve: rgb radiance in
// HDR target units, a confidence that is the roughness fade alone.

#define RAY_TEXTURED_ALPHA
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "pbr_common.slang"
#include "pre_exposure.slang"
#include "ssr_half_res.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"

// VulkanSsrTracePass's set, here at set 2 (set 1 is the ray scene).
[[vk::binding(0, 2)]] Sampler2D depthTexture;   // nearest
[[vk::binding(1, 2)]] Sampler2D normalTexture;  // GB1, nearest
[[vk::binding(2, 2)]] Sampler2D surfaceTexture; // GB2, nearest
[[vk::binding(3, 2)]] Sampler2D historyTexture; // TAA history, last frame, linear
[[vk::binding(4, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ssrRawImage;
[[vk::binding(5, 2)]] Sampler2D coatTexture;     // GB6, nearest
[[vk::binding(6, 2)]] Sampler2D velocityTexture; // .ba: a mapped coat's normal, nearest

#include "ssr_lobe.slang"

// SsrPushConstants in engine/renderer/vulkan/ssr_pass.cpp, as ssr_trace.comp has them.
struct SsrConstants
{
    float2 extent;
    float2 invExtent;
    float maxDistance;
    float maxRoughness;
    float historyScale;
    uint frameIndex;
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<SsrConstants> ssr;

// The screen's own colour stands for a hit that lies on the surface the depth buffer holds there
// within this much (plus a share of its distance).
static const float SCREEN_MATCH_BASE = 0.05;
static const float SCREEN_MATCH_PER_METRE = 0.01;
// How far the sun's shadow ray from a reflected point looks.
static const float HIT_SHADOW_DISTANCE = 20000.0;

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// What the sky sends along a direction, as the specular environment has it at its sharpest.
float3 SkyRadiance(float3 direction)
{
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        return SceneAmbientAlong(direction);
    }
    return prefilteredEnvironment.SampleLevel(direction, 0.0).rgb + SceneLightsAmbientAlong(direction);
}

// The share of the shadow caster's light that reaches a reflected point: one ray at its centre.
float HitSunVisibility(float3 position, float3 faceNormal, uint rayId)
{
    int casterIndex = int(ubo.shadowParams.x);
    if (casterIndex < 0)
    {
        return 1.0;
    }
    float3 toSun = normalize(-sceneLights[casterIndex].directionAndType.xyz);
    if (dot(faceNormal, toSun) <= 0.0)
    {
        return 0.0;
    }
    RayHit shadowHit;
    float3 origin = OffsetRayOrigin(position, faceNormal);
    return TraceSceneRayMasked(origin, toSun, 0.0, HIT_SHADOW_DISTANCE, true, rayId, RAY_MASK_SHADOW, shadowHit) ? 0.0 : 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    bool fullResolution = (ssr.flags & SSR_TRACE_FLAG_FULL_RES) != 0u;
    int2 rawPixel = int2(dispatchThreadId.xy);
    if (any(rawPixel >= (fullResolution ? int2(ssr.extent) : SsrHalfResExtent(int2(ssr.extent)))))
    {
        return;
    }
    int2 pixel = fullResolution ? rawPixel : SsrHalfResSourcePixel(rawPixel, ssr.frameIndex, int2(ssr.extent));

    float depth = depthTexture.Load(int3(pixel, 0)).r;
    SsrLobe lobe = ReadSsrLobe(pixel);
    float roughnessFade = SsrRoughnessFade(lobe.roughness, ssr.maxRoughness);
    if (IsFarDepth(depth) || roughnessFade <= 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * ssr.invExtent;
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPosition = world.xyz / world.w;
    float3 N = lobe.normal;
    float3 geoNormal = DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).ba);
    float3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);

    float2 frameOffset = float2(float(ssr.frameIndex % 64u) * 5.588238, float(ssr.frameIndex % 64u) * 7.123);
    float2 u = fullResolution ? SsrWhiteNoise(pixel, ssr.frameIndex)
                            : float2(InterleavedGradientNoise(float2(pixel) + frameOffset), InterleavedGradientNoise(float2(pixel.yx) + frameOffset * 1.37));
    float3 H = SampleGgxVisibleNormal(N, V, lobe.roughness, u);
    float3 R = reflect(-V, H);
    if (dot(R, N) <= 0.0 || dot(R, geoNormal) <= 0.0)
    {
        ssrRawImage[rawPixel] = float4(0.0);
        return;
    }

    float distanceToCamera = length(ubo.cameraWorldPosition.xyz - worldPosition);
    float3 origin = OffsetRayOrigin(worldPosition + geoNormal * (0.002 + 0.0004 * distanceToCamera), geoNormal);
    uint rayId = uint(pixel.x) * 73856093u ^ uint(pixel.y) * 19349663u ^ ssr.frameIndex * 83492791u;
    RayHit hit;
    float3 radiance = float3(0.0);
    if (!TraceSceneRayMasked(origin, R, 0.0, ssr.maxDistance, false, rayId, RAY_MASK_VISIBILITY, hit))
    {
        radiance = SkyRadiance(R) * ubo.exposure.x;
    }
    else
    {
        RayHitSurface surface = RayHitSurfaceOf(hit, origin, R);
        bool shaded = true;

        // Where the screen shows this very point, last frame's anti-aliased image already holds its
        // colour with every effect on it.
        float4 clip = mul(mul(ubo.proj, ubo.view), float4(surface.position, 1.0));
        if (clip.w > 0.0)
        {
            float2 hitUv = clip.xy / clip.w * 0.5 + 0.5;
            if (all(hitUv >= float2(0.0)) && all(hitUv <= float2(1.0)))
            {
                int2 hitPixel = clamp(int2(hitUv * ssr.extent), int2(0), int2(ssr.extent) - 1);
                float sceneDepth = depthTexture.Load(int3(hitPixel, 0)).r;
                float hitDistance = clip.w;
                float sceneDistance = ubo.proj[2][3] / (sceneDepth + ubo.proj[2][2]);
                float4 previousClip = mul(ubo.prevViewProj, float4(surface.position, 1.0));
                float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
                if (!IsFarDepth(sceneDepth) && abs(sceneDistance - hitDistance) < SCREEN_MATCH_BASE + SCREEN_MATCH_PER_METRE * hitDistance &&
                    previousClip.w > 0.0 && all(previousUv >= float2(0.0)) && all(previousUv <= float2(1.0)))
                {
                    radiance = historyTexture.SampleLevel(previousUv, 0.0).rgb * ssr.historyScale;
                    shaded = false;
                }
            }
        }

        if (shaded)
        {
            RayMaterial rayMaterial = rayMaterials[rayInstances[hit.instance].data.z];
            bool doubleSided = (asuint(rayMaterial.emissionFlags.w) & RAY_MATERIAL_DOUBLE_SIDED) != 0u;
            if (!hit.frontFace && !doubleSided)
            {
                // The inside of a closed mesh: dark.
                radiance = float3(0.0);
            }
            else
            {
                // The ray cone's width at the hit: a pixel's footprint where the ray left, widened by
                // a pixel's angle and the lobe's spread over the ray's length.
                float pixelAngle = 2.0 / (abs(ubo.proj[1][1]) * ssr.extent.y);
                float footprint = (distanceToCamera + hit.t) * pixelAngle + hit.t * lobe.roughness * lobe.roughness;
                RayHitShading material = RayHitShadingOf(surface, footprint, abs(dot(surface.normal, R)));
                if ((material.flags & SHADING_FLAG_UNLIT) != 0u)
                {
                    radiance = material.albedo * kFrameBufferUnitsPerExposed;
                }
                else
                {
                    // The cluster grid covers the view frustum only: a point outside it loops over
                    // every local light.
                    float3 ndc = clip.xyz / max(clip.w, 1e-6);
                    shadeAllLocalLights = clip.w <= 0.0 || any(abs(ndc.xy) > float2(1.0));
                    tracedSunShadow = HitSunVisibility(OffsetRayOrigin(surface.position, surface.faceNormal), surface.faceNormal, rayId ^ 0x5bd1e995u);
                    float3 color = ShadeSurface(
                        surface.position, surface.normal, surface.faceNormal, -R,
                        material.albedo, material.metallic, material.roughness, 1.0, material.emissive,
                        NoCoat(), NoSheen(), NoAnisotropy(), NoSpecularOverride(), float4(0.0));
                    radiance = color * ubo.exposure.x;
                }
            }
        }
    }
    if (any(isnan(radiance)) || any(isinf(radiance)))
    {
        radiance = float3(0.0);
    }
    ssrRawImage[rawPixel] = float4(min(radiance, float3(65504.0)), roughnessFade);
}
