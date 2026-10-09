#include "scene_common.slang"
#include "atmosphere_common.slang"

[[vk::binding(0, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> transmittanceOut : register(u0, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 texel = int2(dispatchThreadId.xy);
    if (any(texel >= int2(TRANSMITTANCE_LUT_SIZE)))
    {
        return;
    }
    float viewHeight;
    float cosZenith;
    UvToTransmittanceLutParams((float2(texel) + 0.5) / TRANSMITTANCE_LUT_SIZE, viewHeight, cosZenith);

    // The parameterisation only covers rays that reach the top boundary, so the march always ends
    // there.
    float3 origin = float3(0.0, viewHeight, 0.0);
    float3 direction = float3(sqrt(max(0.0, 1.0 - cosZenith * cosZenith)), cosZenith, 0.0);
    float tMax = RaySphereIntersectNearest(origin, direction, float3(0.0), TopRadius());
    const float stepCount = 40.0;
    float dt = max(tMax, 0.0) / stepCount;
    float3 opticalDepth = float3(0.0);
    for (float step = 0.0; step < stepCount; step += 1.0)
    {
        opticalDepth += SampleMedium(origin + (step + 0.5) * dt * direction).extinction * dt;
    }
    transmittanceOut[texel] = float4(exp(-opticalDepth), 1.0);
}
