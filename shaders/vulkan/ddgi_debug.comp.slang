#ifdef RAY_QUERY
// The hardware variant of ray_tracing_common.slang's TraceSceneRay.
#endif

// The DDGI debug views, written over SceneGi after the one-bounce composite has used it (the tone
// mapping pass shows SceneGi for them):
// - Ray-traced scene (view 14): one primary ray per pixel through the ray scene, showing the hit's ray
//   material albedo lit by the first directional light through a traced shadow ray, plus a fifth of
//   it unlit. Back faces of single-sided surfaces show red, the sky a flat blue. Against the Albedo
//   view it checks the hierarchies, the instances and the ray materials; against the image, the
//   shadow rays.
// - DDGI probes (view 16): the image with every probe of one level (DdgiSettings::probeViewLevel)
//   drawn as a small sphere (a twentieth of the spacing) showing its irradiance toward the viewer, found by walking each level's grid along the
//   pixel's ray up to the surface; inactive probes red, probes not yet updated for where they are
//   blue.
// - DDGI irradiance (view 15): what the probes send each pixel's surface, irradiance / pi pre-exposed
//   like SceneGi, before the albedo; black where the volume does not reach (the SH sky lights it).

#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "scene_common.slang"
#include "ray_tracing_common.slang"
#include "ddgi_common.slang"
#include "ground_plane.slang"
#include "gbuffer_common.slang"

[[vk::binding(10, 0)]] StructuredBuffer<SceneLightData> sceneLights : register(t10, space0);

[[vk::binding(0, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outputImage : register(u0, space2);
[[vk::binding(1, 2)]] Texture2D depthTexture : register(t1, space2);
[[vk::binding(65, 2)]] SamplerState depthTextureSampler : register(s65, space2);
[[vk::binding(2, 2)]] Texture2D normalTexture : register(t2, space2);
[[vk::binding(66, 2)]] SamplerState normalTextureSampler : register(s66, space2);

// Must match DdgiDebugConstants in engine/renderer/vulkan/ddgi_debug_pass.cpp.
struct DdgiDebugConstants
{
    float2 extent;
    float2 invExtent;
    uint view;
    uint frameIndex;
};
[[vk::push_constant]] ConstantBuffer<DdgiDebugConstants> constants D3D_PUSH_CONSTANTS(2);

static const uint VIEW_RAY_TRACED = 14u;
static const uint VIEW_DDGI_IRRADIANCE = 15u;
static const uint VIEW_DDGI_PROBES = 16u;

[[vk::binding(3, 2)]] Texture2D hdrTexture : register(t3, space2);
[[vk::binding(67, 2)]] SamplerState hdrTextureSampler : register(s67, space2);

// The nearest probe sphere of one level the ray meets before maxT: walks the level's cells along the
// ray (Amanatides and Woo) and tests the eight probes around each.
bool FindProbe(uint level, float3 origin, float3 direction, float maxT, inout float bestT, out float3 color)
{
    color = float3(0.0);
    float spacing = DdgiLevelSpacing(level);
    int3 gridOrigin = DdgiLevelOrigin(level);
    float radius = 0.05 * spacing;
    float3 start = origin / spacing - float3(gridOrigin);
    float3 inverse = SafeInverse(direction);
    // Enter the grid's box.
    float3 t0 = (float3(0.0) - start) * inverse;
    float3 t1 = (float3(DDGI_GRID - 1) - start) * inverse;
    float enter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float exit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), min(max(t0.z, t1.z), maxT / spacing));
    if (enter > exit)
    {
        return false;
    }
    float3 position = start + direction * enter;
    int3 cell = clamp(int3(floor(position)), int3(0), DDGI_GRID - 2);
    int3 stepDirection = int3(sign(direction));
    float3 nextBoundary = (float3(cell) + max(float3(stepDirection), float3(0.0)) - start) * inverse;
    float3 delta = abs(inverse);
    bool found = false;
    for (int steps = 0; steps < 96; ++steps)
    {
        for (int corner = 0; corner < 8; ++corner)
        {
            int3 coord = gridOrigin + cell + int3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
            int3 slot = DdgiStorageSlot(coord);
            DdgiProbeState state = ddgiProbeStates[level * DDGI_PROBES_PER_LEVEL + DdgiSlotIndex(slot)];
            float3 center = DdgiProbePosition(level, coord, state);
            float3 toCenter = center - origin;
            float along = dot(toCenter, direction);
            float squared = dot(toCenter, toCenter) - along * along;
            if (squared > radius * radius)
            {
                continue;
            }
            float t = along - sqrt(radius * radius - squared);
            if (t <= 0.0 || t >= bestT || t >= maxT)
            {
                continue;
            }
            bestT = t;
            found = true;
            bool current = all(state.coordAndFlags.xyz == coord) && (state.coordAndFlags.w & DDGI_PROBE_UPDATED) != 0;
            if (!current)
            {
                color = float3(0.0, 0.0, 1.0);
            }
            else if ((state.coordAndFlags.w & DDGI_PROBE_INACTIVE) != 0)
            {
                color = float3(1.0, 0.0, 0.0);
            }
            else
            {
                float3 normal = normalize(origin + direction * t - center);
                float2 size = float2(TextureSize(ddgiIrradianceAtlas, 0).xy);
                float3 irradiance = ddgiIrradianceAtlas.SampleLevel(ddgiIrradianceAtlasSampler, float3(DdgiAtlasUv(slot, normal, DDGI_IRRADIANCE_TEXELS, size), float(level)), 0.0).rgb;
                // Pre-exposed and scaled back to the display's unit, as the tone mapping pass shows SceneGi.
                color = irradiance * ubo.exposure.x;
            }
        }
        if (found)
        {
            return true;
        }
        // To the next cell.
        if (nextBoundary.x < nextBoundary.y && nextBoundary.x < nextBoundary.z)
        {
            cell.x += stepDirection.x;
            nextBoundary.x += delta.x;
        }
        else if (nextBoundary.y < nextBoundary.z)
        {
            cell.y += stepDirection.y;
            nextBoundary.y += delta.y;
        }
        else
        {
            cell.z += stepDirection.z;
            nextBoundary.z += delta.z;
        }
        // A sphere reaches a cell beyond its own, so the walk goes one cell past the exit.
        if (any(cell < int3(0)) || any(cell > DDGI_GRID - 2) || min(min(nextBoundary.x, nextBoundary.y), nextBoundary.z) > exit + 1.0)
        {
            return false;
        }
    }
    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(float2(pixel) >= constants.extent))
    {
        return;
    }
    float2 uv = (float2(pixel) + 0.5) * constants.invExtent;
    if (constants.view == VIEW_DDGI_PROBES)
    {
        float depth = depthTexture.SampleLevel(depthTextureSampler, uv, 0.0).r;
        float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
        float4 far = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, DEPTH_FAR, 1.0));
        float3 origin = ubo.cameraWorldPosition.xyz;
        float3 direction = normalize(far.xyz / far.w - origin);
        float maxT = !IsFarDepth(depth) ? length(world.xyz / world.w - origin) : 1e30;
        float3 color = hdrTexture.SampleLevel(hdrTextureSampler, uv, 0.0).rgb;
        float bestT = maxT;
        uint level = uint(ubo.ddgiParams.y);
        float3 probeColor;
        if (level < DdgiLevelCount() && FindProbe(level, origin, direction, maxT, bestT, probeColor))
        {
            color = probeColor;
        }
        outputImage[pixel] = float4(color, 1.0);
        return;
    }
    if (constants.view == VIEW_DDGI_IRRADIANCE)
    {
        float depth = depthTexture.SampleLevel(depthTextureSampler, uv, 0.0).r;
        float3 irradiance = float3(0.0);
        if (!IsFarDepth(depth) && DdgiLevelCount() > 0u)
        {
            float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
            float3 P = world.xyz / world.w;
            float3 N = DecodeNormalOctahedral(normalTexture.SampleLevel(normalTextureSampler, uv, 0.0).rg);
            float weight;
            irradiance = DdgiIrradiance(P, N, normalize(ubo.cameraWorldPosition.xyz - P), weight).rgb * ubo.exposure.x;
        }
        outputImage[pixel] = float4(irradiance, 1.0);
        return;
    }
    float4 farPoint = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, DEPTH_FAR, 1.0));
    float3 origin = ubo.cameraWorldPosition.xyz;
    float3 direction = normalize(farPoint.xyz / farPoint.w - origin);
    uint rayId = uint(pixel.y) * uint(constants.extent.x) + uint(pixel.x);

    float3 color = float3(0.25, 0.4, 0.7);
    RayHit hit;
    bool sceneHit = TraceSceneRay(origin, direction, 0.0, 1e30, false, rayId, hit);
    float groundT;
    bool groundFrontFace;
    bool groundHit = IntersectGroundPlane(origin, direction, 0.0, sceneHit ? hit.t : 1e30, groundT, groundFrontFace);
    if (sceneHit || groundHit)
    {
        float3 albedo;
        float3 normal;
        bool frontFace;
        float t;
        if (groundHit)
        {
            albedo = ubo.groundAlbedo.rgb;
            normal = float3(0.0, 1.0, 0.0);
            frontFace = groundFrontFace;
            t = groundT;
        }
        else
        {
            RayMaterial material = RayHitMaterial(hit);
            albedo = material.albedoCoverage.rgb;
            normal = RayHitNormal(hit);
            frontFace = hit.frontFace || (asuint(material.emissionFlags.w) & RAY_MATERIAL_DOUBLE_SIDED) != 0u;
            t = hit.t;
        }
        if (!frontFace)
        {
            color = float3(0.8, 0.05, 0.05);
        }
        else
        {
            normal = dot(normal, direction) > 0.0 ? -normal : normal;
            float3 position = origin + direction * t;
            float lit = 0.0;
            if (ubo.lightCounts.x > 0u)
            {
                float3 toLight = -normalize(sceneLights[0].directionAndType.xyz);
                float cosine = max(dot(normal, toLight), 0.0);
                RayHit shadowHit;
                if (cosine > 0.0 && !TraceSceneShadowRay(OffsetRayOrigin(position, normal), toLight, 0.0, 1e30, rayId ^ 0x9e3779b9u, shadowHit))
                {
                    lit = cosine;
                }
            }
            color = albedo * (0.2 + 0.8 * lit);
        }
    }
    outputImage[pixel] = float4(color, 1.0);
}
