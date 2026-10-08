// The guides DLSS ray reconstruction denoises with (VulkanTaaPass, docs/design/
// 2026-10-07-ray-traced-effects-design.md), from the G-buffer at the render size: the diffuse albedo
// (the base colour without what metal and the dielectric's reflection take), the specular albedo (the
// split-sum directional albedo of the specular lobe, Karis's analytic fit) and the world space
// shading normal with the perceptual roughness in w (NVSDK_NGX_DLSS_Roughness_Mode_Packed). Pixels no
// geometry covered get zero albedos and a normal toward the camera.
// With the plain path tracer's raw paths (guide.hitDistance), also the specular ray's hit distance (the
// path tracer's specular result carries it in its alpha) and the motion vectors of what the
// reflection shows: the virtual image's, the point as far behind the surface along the view ray as the
// ray went in front of it, reprojected through last frame's camera (exact for a flat mirror and a
// still scene), so a glossy reflection is not dragged along with the surface it is seen in.

#include "scene_common.slang"
#include "gbuffer_common.slang"

[[vk::binding(0, 1)]] Sampler2D albedoTexture;   // GB0
[[vk::binding(1, 1)]] Sampler2D normalTexture;   // GB1
[[vk::binding(2, 1)]] Sampler2D surfaceTexture;  // GB2
[[vk::binding(3, 1)]] Sampler2D specularTexture; // GB5
[[vk::binding(4, 1)]] Sampler2D depthTexture;
[[vk::binding(5, 1)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> diffuseAlbedoImage;
[[vk::binding(6, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> specularAlbedoImage;
[[vk::binding(7, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> normalRoughnessImage;
[[vk::binding(8, 1)]] Sampler2D reflectionsTexture;  // the path tracer's specular result
[[vk::binding(9, 1)]] [[vk::image_format("r16f")]] RWTexture2D<float> hitDistanceImage;
[[vk::binding(10, 1)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> reflectionMotionImage;

// DlssMotionPushConstants in engine/renderer/vulkan/taa_pass.cpp, shared with dlss_motion_vectors.comp.
struct GuideConstants
{
    float2 extent;
    float2 invExtent;
    float2 jitterPixels;
    // x: 1 to write the hit distance and the reflections' motion vectors.
    float2 hitDistance;
};
[[vk::push_constant]] ConstantBuffer<GuideConstants> guide;

// The split sum's (scale, bias) for the GGX lobe (Karis 2014, "Physically Based Shading on Mobile").
float2 EnvironmentBrdfApprox(float roughness, float NdV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(guide.extent)))
    {
        return;
    }
    float2 uv = (float2(pixel) + 0.5) * guide.invExtent;
    float depth = depthTexture.Load(int3(pixel, 0)).r;
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, IsFarDepth(depth) ? DEPTH_FAR : depth, 1.0));
    float3 V = normalize(ubo.cameraWorldPosition.xyz - world.xyz / world.w);
    if (guide.hitDistance.x > 0.0)
    {
        // As dlss_motion_vectors.comp: from where the point is now (unjittered) to where it was, in
        // render pixels. The sky (and a pixel without a specular path, distance 0) moves as the
        // surface point would if it stood still.
        float distance = IsFarDepth(depth) ? 0.0 : reflectionsTexture.Load(int3(pixel, 0)).a;
        float3 surface = world.xyz / world.w;
        float3 virtualPoint = surface + normalize(surface - ubo.cameraWorldPosition.xyz) * distance;
        float4 previousClip = mul(ubo.prevViewProj, float4(virtualPoint, 1.0));
        float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
        float2 currentUv = uv - guide.jitterPixels * guide.invExtent;
        hitDistanceImage[pixel] = distance;
        reflectionMotionImage[pixel] = (previousUv - currentUv) * guide.extent;
    }
    if (IsFarDepth(depth))
    {
        diffuseAlbedoImage[pixel] = float4(0.0);
        specularAlbedoImage[pixel] = float4(0.0);
        normalRoughnessImage[pixel] = float4(V, 1.0);
        return;
    }

    float3 albedo = albedoTexture.Load(int3(pixel, 0)).rgb;
    float3 N = DecodeNormalOctahedral(normalTexture.Load(int3(pixel, 0)).rg);
    float4 surface = surfaceTexture.Load(int3(pixel, 0));
    float metallic = surface.r;
    float roughness = clamp(surface.g, 0.04, 1.0);
    float3 dielectricF0 = float3(0.04);
    if ((DecodeShadingFlags(surface.a) & SHADING_FLAG_SPECULAR) != 0u)
    {
        float3 stored = specularTexture.Load(int3(pixel, 0)).rgb;
        dielectricF0 = stored * stored;
    }
    float3 F0 = lerp(dielectricF0, albedo, metallic);
    float2 split = EnvironmentBrdfApprox(roughness, max(dot(N, V), 1e-4));
    float3 specularAlbedo = clamp(F0 * split.x + split.y, 0.0, 1.0);
    float3 diffuseAlbedo = albedo * (1.0 - metallic);

    diffuseAlbedoImage[pixel] = float4(diffuseAlbedo, 1.0);
    specularAlbedoImage[pixel] = float4(specularAlbedo, 1.0);
    normalRoughnessImage[pixel] = float4(N, roughness);
}
