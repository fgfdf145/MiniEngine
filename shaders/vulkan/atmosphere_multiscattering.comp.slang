#include "scene_common.slang"
#include "atmosphere_common.slang"
#include "atmosphere_integrate.slang"

// One workgroup per texel; its 64 invocations each integrate one direction of an 8 x 8 grid that
// is uniform over the sphere, then reduce.
[[vk::binding(1, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> multiScatteringOut;
[[vk::binding(4, 1)]] Sampler2D transmittanceLut;
[[vk::binding(5, 1)]] Sampler2D multiScatteringLut;

groupshared float3 sharedMultiScatteringAs1[64];
groupshared float3 sharedLuminance[64];

[shader("compute")]
[numthreads(1, 1, 64)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    int2 texel = int2(groupId.xy);
    uint index = groupThreadId.z;

    float2 uv = FromSubUvsToUnit((float2(texel) + 0.5) / MULTI_SCATTERING_LUT_SIZE, MULTI_SCATTERING_LUT_SIZE);
    float cosSunZenith = clamp(uv.x * 2.0 - 1.0, -1.0, 1.0);
    float viewHeight = BottomRadius() + clamp(uv.y + PLANET_RADIUS_OFFSET_KM, 0.0, 1.0) * (TopRadius() - BottomRadius() - PLANET_RADIUS_OFFSET_KM);
    float3 origin = float3(0.0, viewHeight, 0.0);
    float3 sunDirection = float3(0.0, cosSunZenith, -sqrt(max(0.0, 1.0 - cosSunZenith * cosSunZenith)));

    float i = 0.5 + float(index / 8u);
    float j = 0.5 + float(index % 8u);
    float theta = 2.0 * ATMOSPHERE_PI * i / 8.0;
    float phi = acos(1.0 - 2.0 * j / 8.0);
    float3 direction = float3(cos(theta) * sin(phi), cos(phi), sin(theta) * sin(phi));

    ScatteringResult result = IntegrateScatteredLuminance(
        origin, direction, sunDirection, float3(1.0), AtmosphereLut(transmittanceLut), AtmosphereLut(multiScatteringLut),
        true, 20.0, 9.0e9, false, false);
    sharedMultiScatteringAs1[index] = result.multiScatteringAs1 / 64.0;
    sharedLuminance[index] = result.luminance / 64.0;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            sharedMultiScatteringAs1[index] += sharedMultiScatteringAs1[index + stride];
            sharedLuminance[index] += sharedLuminance[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0u)
    {
        // The isotropic phase times the sphere's solid angle is 1, so the averages are the
        // second-order luminance and f_ms; every further order is a geometric series.
        float3 fms = sharedMultiScatteringAs1[0];
        float3 luminance = sharedLuminance[0] / max(float3(1.0) - fms, float3(1e-4));
        multiScatteringOut[texel] = float4(luminance, 1.0);
    }
}
