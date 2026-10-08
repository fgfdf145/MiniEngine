// The ray traced sun shadow's trace (VulkanRtShadowPass): one ray a pixel from the surface toward a
// point on the shadow caster's disk, the size of the environment's sun, so the shadows are as soft
// as the sun makes them once the filters have averaged the rays. Alpha-tested surfaces are tested
// against their textures; moving instances cast shadows too. Writes ShadowRaw: the visibility and,
// where the ray was stopped, how far away its occluder is, which sizes the penumbra filter.

#define RAY_TEXTURED_ALPHA
#define RT_SHADOW_SET 2
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "rt_shadow_common.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"

[[vk::binding(10, 0)]] StructuredBuffer<SceneLightData> sceneLights;

static const float PI = 3.14159265359;
// How far a shadow ray looks: past every map the engine loads.
static const float MAX_SHADOW_DISTANCE = 20000.0;

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(rtShadow.extent)))
    {
        return;
    }
    float depth = rtShadowDepth.Load(int3(pixel, 0)).r;
    int casterIndex = int(ubo.shadowParams.x);
    if (IsFarDepth(depth) || casterIndex < 0)
    {
        rtShadowRaw[pixel] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 position = RtShadowWorldPosition(pixel, depth);
    float3 normal = DecodeNormalOctahedral(rtShadowNormal.Load(int3(pixel, 0)).ba);
    float3 toSun = normalize(-sceneLights[casterIndex].directionAndType.xyz);

    // A direction within the sun's disk: uniform over the cone's solid angle, stratified per pixel by
    // interleaved gradient noise that the frame index moves, a hash for the angle around it.
    float cosRadius = clamp(ubo.sunIlluminance.w, 0.99, 1.0);
    float2 noisePixel = float2(pixel) + 5.588238 * float(rtShadow.frameIndex % 64u);
    float u1 = InterleavedGradientNoise(noisePixel);
    float u2 = RayHash(uint(pixel.x) + uint(pixel.y) * 65536u, rtShadow.frameIndex);
    float cosTheta = lerp(1.0, cosRadius, u1);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float3 helper = abs(toSun.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, toSun));
    float3 bitangent = cross(toSun, tangent);
    float phi = 2.0 * PI * u2;
    float3 direction = normalize(toSun * cosTheta + (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta);

    // Off the surface on the sun's side (a thin double-sided surface lit from behind leaves from its
    // back), by more than the depth buffer's reconstruction error, which grows with distance.
    float3 offsetNormal = dot(normal, toSun) >= 0.0 ? normal : -normal;
    float distanceToCamera = RtShadowViewDistance(depth);
    float3 origin = OffsetRayOrigin(position + offsetNormal * (0.002 + 0.0004 * distanceToCamera), offsetNormal);

    RayHit hit;
    uint rayId = uint(pixel.x) * 73856093u ^ uint(pixel.y) * 19349663u ^ rtShadow.frameIndex * 83492791u;
    bool blocked = TraceSceneRayMasked(origin, direction, 0.0, MAX_SHADOW_DISTANCE, true, rayId, RAY_MASK_SHADOW, hit);
    rtShadowRaw[pixel] = float4(blocked ? 0.0 : 1.0, blocked ? hit.t : 0.0, 0.0, 0.0);
}
