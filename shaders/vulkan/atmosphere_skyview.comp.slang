#include "scene_common.slang"
#include "atmosphere_common.slang"
#include "atmosphere_integrate.slang"

[[vk::binding(2, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> skyViewOut : register(u2, space1);
// Set 1 is VulkanAtmosphere's NVRHI binding set: each sampled LUT's sampler at its binding + 64.
[[vk::binding(4, 1)]] Texture2D transmittanceLut : register(t4, space1);
[[vk::binding(68, 1)]] SamplerState transmittanceLutSampler : register(s68, space1);
[[vk::binding(5, 1)]] Texture2D multiScatteringLut : register(t5, space1);
[[vk::binding(69, 1)]] SamplerState multiScatteringLutSampler : register(s69, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 texel = int2(dispatchThreadId.xy);
    if (any(texel >= int2(SKY_VIEW_LUT_SIZE)))
    {
        return;
    }
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    float viewZenithCos;
    float lightViewCos;
    UvToSkyViewLutParams((float2(texel) + 0.5) / SKY_VIEW_LUT_SIZE, viewHeight, viewZenithCos, lightViewCos);

    // A local frame with +Y up and the sun's azimuth along +X; only the angles matter.
    float cosSunZenith = dot(camera / viewHeight, ubo.sunDirectionAndMode.xyz);
    float3 sunDirection = normalize(float3(sqrt(max(0.0, 1.0 - cosSunZenith * cosSunZenith)), cosSunZenith, 0.0));
    float viewZenithSin = sqrt(max(0.0, 1.0 - viewZenithCos * viewZenithCos));
    float3 direction = float3(
        viewZenithSin * lightViewCos,
        viewZenithCos,
        viewZenithSin * sqrt(max(0.0, 1.0 - lightViewCos * lightViewCos)));

    ScatteringResult result = IntegrateScatteredLuminance(
        float3(0.0, viewHeight, 0.0), direction, sunDirection, ubo.sunIlluminance.rgb,
        AtmosphereLut(transmittanceLut, transmittanceLutSampler), AtmosphereLut(multiScatteringLut, multiScatteringLutSampler), false, 30.0, 9.0e9, true, true);
    skyViewOut[texel] = float4(result.luminance, 1.0);
}
