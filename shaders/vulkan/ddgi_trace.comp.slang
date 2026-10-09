#ifdef RAY_QUERY
// The hardware variant of ray_tracing_common.slang's TraceSceneRay.
#endif

// The DDGI probe rays: one workgroup per scheduled probe, one invocation per ray. Each ray leaves the
// probe along a spherical Fibonacci direction turned by this frame's rotation and returns the
// radiance it brings back and how far it went (VulkanDdgi's ray buffer):
// - a miss: the sky along the ray (the prefiltered cube at a low mip under a physical sky, plus the
//   Ambient and Hemisphere lights; the ambient under None), distance DDGI_MISS_DISTANCE;
// - a back face of a single-sided surface: black, the distance negated and shortened to a fifth (the
//   probe may be inside it; the visibility moments then keep the probe from lighting past it);
// - a front face: its emission, plus its albedo over pi times the direct light (directional lights
//   through a traced shadow ray; point and spot lights unshadowed), plus its albedo times the probes'
//   own irradiance there: every bounce, one per frame.
// The ground plane (ground_plane.slang), when drawn, is a front face like any other, and its underside
// a single-sided back face.

#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "spherical_harmonics.slang"
#include "ray_tracing_common.slang"
#include "ddgi_common.slang"
#include "ground_plane.slang"

[[vk::binding(10, 0)]] StructuredBuffer<SceneLightData> sceneLights : register(t10, space0);

[[vk::binding(0, 2)]] StructuredBuffer<uint> scheduledProbes : register(t0, space2);
[[vk::binding(1, 2)]] RWStructuredBuffer<float4> rays : register(u1, space2);

// Must match DdgiTraceConstants in engine/renderer/vulkan/ddgi.cpp.
struct DdgiTraceConstants
{
    float4 rotation[3]; // this frame's ray rotation, columns, xyz used
    uint scheduledCount;
    uint frameIndex;
};
[[vk::push_constant]] ConstantBuffer<DdgiTraceConstants> constants D3D_PUSH_CONSTANTS(2);

static const float PI = 3.14159265359;
static const float DDGI_MISS_DISTANCE = 1e27;
// The prefiltered cube's mip for a missed ray: roughness 0.2, blurred enough that 64 rays per frame
// do not sample single bright texels.
static const float MISS_MIP = 1.0;
// Radiance a hit or a miss may return, cd/m^2: a lamp's bulb seen by one ray must not flash the
// probe.
static const float MAX_RAY_RADIANCE = 2e5;

// The index-th of count points of the spherical Fibonacci set (Keinert et al. 2015).
float3 SphericalFibonacci(float index, float count)
{
    const float goldenRatio = 1.61803398875;
    float phi = 2.0 * PI * frac(index * (goldenRatio - 1.0));
    float cosTheta = 1.0 - (2.0 * index + 1.0) / count;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return float3(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta);
}

float3 SkyRadiance(float3 direction)
{
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        return SceneAmbientAlong(direction);
    }
    return prefilteredEnvironment.SampleLevel(prefilteredEnvironmentSampler, direction, MISS_MIP).rgb + SceneLightsAmbientAlong(direction);
}

// pbr_common.slang's RangeWindow and SmoothDistanceAttenuation, which cannot be included in compute.
float LightRangeWindow(float distance, float range)
{
    float ratio = distance / max(range, 0.001);
    float ratio4 = ratio * ratio * ratio * ratio;
    float num = clamp(1.0 - ratio4, 0.0, 1.0);
    return num * num;
}

// The illuminance a local light puts on a surface facing N at P (point and spot; area lights are
// left out), unshadowed.
float3 LocalLightIrradiance(SceneLightData light, float3 P, float3 N)
{
    int type = int(light.directionAndType.w);
    float3 toLight = light.positionAndRange.xyz - P;
    float distance = length(toLight);
    if (distance >= light.positionAndRange.w || (type != LIGHT_POINT && type != LIGHT_SPOT))
    {
        return float3(0.0);
    }
    float3 L = toLight / max(distance, 1e-4);
    float cosine = dot(N, L);
    if (cosine <= 0.0)
    {
        return float3(0.0);
    }
    float attenuation = LightRangeWindow(distance, light.positionAndRange.w) / max(distance * distance, 1e-4);
    float intensity = light.colorAndIntensity.w / (4.0 * PI);
    if (type == LIGHT_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.directionAndType.xyz));
        float innerCos = light.spotAndArea.x;
        float outerCos = light.spotAndArea.y;
        float spot = clamp((cosAngle - outerCos) / max(innerCos - outerCos, 1e-4), 0.0, 1.0);
        intensity = light.colorAndIntensity.w / max(2.0 * PI * (1.0 - outerCos), 1e-4) * spot * spot;
    }
    return light.colorAndIntensity.rgb * intensity * attenuation * cosine;
}

// The sky's irradiance / pi where the volume does not reach: the SH sky and the ambient lights.
float3 FallbackIrradiance(float3 N)
{
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        return SceneAmbientAlong(N);
    }
    float basis[9];
    EvaluateShBasis(N, basis);
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    float3 irradiance = float3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        float3 coefficient = hdri ? ubo.hdriIrradianceSh[i].rgb : skyIrradiance[0].coefficients[i].rgb;
        irradiance += coefficient * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, float3(0.0)) / PI + SceneLightsAmbientAlong(N);
}

// A front-face hit t along the ray, with normal N (facing the ray): its radiance back along the
// ray, and t.
float4 ShadeRayHit(float3 origin, float3 direction, float t, float3 N, float3 albedo, float3 emission, uint rayId)
{
    float3 P = origin + direction * t;
    float3 shadowOrigin = OffsetRayOrigin(P, N);

    float3 irradiance = float3(0.0);
    for (uint index = 0u; index < ubo.lightCounts.x; ++index)
    {
        SceneLightData light = sceneLights[index];
        float3 L = -normalize(light.directionAndType.xyz);
        float cosine = dot(N, L);
        RayHit shadowHit;
        if (cosine > 0.0 && !TraceSceneShadowRay(shadowOrigin, L, 0.0, 1e30, rayId ^ 0x9e3779b9u, shadowHit))
        {
            // The shadow caster is the sun, which the clouds shade too.
            float clouds = int(index) == int(ubo.shadowParams.x) ? CloudShadow(P) : 1.0;
            irradiance += light.colorAndIntensity.rgb * light.colorAndIntensity.w * cosine * clouds;
        }
    }
    for (uint index = ubo.lightCounts.x; index < ubo.lightCounts.y; ++index)
    {
        irradiance += LocalLightIrradiance(sceneLights[index], P, N);
    }

    float volumeWeight;
    float3 bounce = DdgiIrradiance(P, N, -direction, volumeWeight).rgb;
    bounce += (1.0 - volumeWeight) * FallbackIrradiance(N);

    float3 radiance = emission + albedo * (irradiance / PI + bounce);
    return float4(min(radiance, float3(MAX_RAY_RADIANCE)), t);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    uint probe = groupId.x;
    uint rayIndex = groupThreadId.x;
    if (probe >= constants.scheduledCount)
    {
        return;
    }
    uint packed = scheduledProbes[probe];
    uint level = packed >> 24;
    int3 slot = DdgiSlotFromIndex(packed & 0xffffffu);
    int3 coord = DdgiSlotCoordinate(slot, DdgiLevelOrigin(level));
    DdgiProbeState state = ddgiProbeStates[level * DDGI_PROBES_PER_LEVEL + DdgiSlotIndex(slot)];
    float3 origin = DdgiProbePosition(level, coord, state);

    float3x3 rotation = MatrixFromColumns(constants.rotation[0].xyz, constants.rotation[1].xyz, constants.rotation[2].xyz);
    float3 direction = normalize(mul(rotation, SphericalFibonacci(float(rayIndex), 64.0)));
    uint rayId = (constants.frameIndex * 4096u + probe) * 64u + rayIndex;

    float4 result;
    RayHit hit;
    bool sceneHit = TraceSceneRay(origin, direction, 0.0, 1e30, false, rayId, hit);
    float groundT;
    bool groundFrontFace;
    bool groundHit = IntersectGroundPlane(origin, direction, 0.0, sceneHit ? hit.t : 1e30, groundT, groundFrontFace);
    if (groundHit)
    {
        result = groundFrontFace
                     ? ShadeRayHit(origin, direction, groundT, float3(0.0, 1.0, 0.0), ubo.groundAlbedo.rgb, float3(0.0), rayId)
                     : float4(0.0, 0.0, 0.0, -0.2 * groundT);
    }
    else if (!sceneHit)
    {
        result = float4(min(SkyRadiance(direction), float3(MAX_RAY_RADIANCE)), DDGI_MISS_DISTANCE);
    }
    else
    {
        RayMaterial material = RayHitMaterial(hit);
        bool doubleSided = (asuint(material.emissionFlags.w) & RAY_MATERIAL_DOUBLE_SIDED) != 0u;
        if (!hit.frontFace && !doubleSided)
        {
            result = float4(0.0, 0.0, 0.0, -0.2 * hit.t);
        }
        else
        {
            float3 N = RayHitNormal(hit);
            N = dot(N, direction) > 0.0 ? -N : N;
            result = ShadeRayHit(origin, direction, hit.t, N, material.albedoCoverage.rgb, material.emissionFlags.rgb, rayId);
        }
    }
    rays[probe * 64u + rayIndex] = result;
}
