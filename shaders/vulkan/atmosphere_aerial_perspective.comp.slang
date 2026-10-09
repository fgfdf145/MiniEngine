#include "scene_common.slang"
#include "atmosphere_common.slang"
#include "atmosphere_integrate.slang"

[[vk::binding(3, 1)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> aerialPerspectiveOut : register(u3, space1);
// Set 1 is VulkanAtmosphere's NVRHI binding set: each sampled LUT's sampler at its binding + 64.
[[vk::binding(4, 1)]] Texture2D transmittanceLut : register(t4, space1);
[[vk::binding(68, 1)]] SamplerState transmittanceLutSampler : register(s68, space1);
[[vk::binding(5, 1)]] Texture2D multiScatteringLut : register(t5, space1);
[[vk::binding(69, 1)]] SamplerState multiScatteringLutSampler : register(s69, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int3 texel = int3(dispatchThreadId.xyz);
    // The volume covers the view frustum: xy the screen, z the distance (see
    // AERIAL_PERSPECTIVE_KM_PER_SLICE for the quadratic spread).
    float2 uv = (float2(texel.xy) + 0.5) / AERIAL_PERSPECTIVE_SLICE_COUNT;
    float4 farPoint = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, DEPTH_FAR, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - ubo.cameraWorldPosition.xyz);
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;

    float slice = (float(texel.z) + 0.5) / AERIAL_PERSPECTIVE_SLICE_COUNT;
    slice = slice * slice * AERIAL_PERSPECTIVE_SLICE_COUNT;
    float tMax = slice * AERIAL_PERSPECTIVE_KM_PER_SLICE;
    float3 end = camera + tMax * direction;
    if (length(end) <= BottomRadius() + PLANET_RADIUS_OFFSET_KM)
    {
        // Below the ground: aim at the point on the ground instead, so the slice still holds
        // plausible haze for geometry drawn there.
        end = normalize(end) * (BottomRadius() + PLANET_RADIUS_OFFSET_KM);
        direction = normalize(end - camera);
        tMax = length(end - camera);
    }

    ScatteringResult result = IntegrateScatteredLuminance(
        camera, direction, ubo.sunDirectionAndMode.xyz, ubo.sunIlluminance.rgb,
        AtmosphereLut(transmittanceLut, transmittanceLutSampler), AtmosphereLut(multiScatteringLut, multiScatteringLutSampler), false, max(1.0, float(texel.z + 1) * 2.0), tMax, true, true);
    float meanTransmittance = dot(result.transmittance, float3(1.0 / 3.0));
    aerialPerspectiveOut[texel] = float4(result.luminance, meanTransmittance);
}
