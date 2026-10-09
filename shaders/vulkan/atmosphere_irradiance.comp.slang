#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "spherical_harmonics.slang"

// One workgroup: 64 invocations, 128 directions each, of a 128 x 64 grid uniform in azimuth and
// cos zenith, so every direction stands for the same solid angle, 4 pi / 8192.
struct SkyIrradianceOut
{
    float4 coefficients[9];
};
[[vk::binding(6, 1)]] RWStructuredBuffer<SkyIrradianceOut> irradianceOut : register(u6, space1); // one element

groupshared float3 sharedSums[64][9];
// The SH of the ground's view transmittance times its diffuse share over the directions that meet it, and the sky's
// illuminance on level ground: the ground's skylight is their product times albedo / pi, which
// only the finished sum knows, so it is added after the reduction.
groupshared float3 sharedGround[64][9];
groupshared float3 sharedSkyUp[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint groupIndex : SV_GroupIndex)
{
    uint index = groupIndex;
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float3 up = normalize(camera);
    float3 sums[9];
    float3 ground[9];
    for (int i = 0; i < 9; ++i)
    {
        sums[i] = float3(0.0);
        ground[i] = float3(0.0);
    }
    float3 skyUp = float3(0.0);
    for (uint sampleIndex = index; sampleIndex < 8192u; sampleIndex += 64u)
    {
        float u = (float(sampleIndex % 128u) + 0.5) / 128.0;
        float v = (float(sampleIndex / 128u) + 0.5) / 64.0;
        float phi = 2.0 * ATMOSPHERE_PI * u;
        float cosTheta = 1.0 - 2.0 * v;
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        float3 direction = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
        // SampleSkyForLighting without the ground's skylight and sky reflection, which read this
        // buffer: the skylight is added after the reduction; the reflection, a few percent of the
        // ground's light away from grazing angles, is left out.
        bool intersectGround;
        float3 radiance = SampleSkyViewLut(direction, intersectGround);
        float3 groundWeight = float3(0.0);
        if (intersectGround && SeamlessHorizon())
        {
            // No ground: the sky mirrored across the horizon, and no skylight bounced off it.
            radiance = SampleFoldedSkyViewLut(direction);
        }
        else if (intersectGround)
        {
            float3 groundUp;
            groundWeight = GroundViewTransmittance(direction, groundUp);
            radiance += GroundSurfaceRadiance(direction, groundUp, GroundSunIrradiance(groundUp), float3(0.0), float3(0.0)) * groundWeight;
            // The skylight's diffuse share, which the reduction scales by the sky's illuminance.
            groundWeight *= 1.0 - GroundSpecularAlbedo(direction, groundUp);
        }
        else
        {
            skyUp += radiance * max(dot(direction, up), 0.0);
        }
        float basis[9];
        EvaluateShBasis(direction, basis);
        for (int i = 0; i < 9; ++i)
        {
            sums[i] += radiance * basis[i];
            ground[i] += groundWeight * basis[i];
        }
    }
    for (int i = 0; i < 9; ++i)
    {
        sharedSums[index][i] = sums[i];
        sharedGround[index][i] = ground[i];
    }
    sharedSkyUp[index] = skyUp;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            for (int i = 0; i < 9; ++i)
            {
                sharedSums[index][i] += sharedSums[index + stride][i];
                sharedGround[index][i] += sharedGround[index + stride][i];
            }
            sharedSkyUp[index] += sharedSkyUp[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index < 9u)
    {
        const float solidAngle = 4.0 * ATMOSPHERE_PI / 8192.0;
        float3 groundSkyIrradiance = sharedSkyUp[0] * solidAngle;
        float3 groundSkylight = ubo.groundAlbedo.rgb / ATMOSPHERE_PI * groundSkyIrradiance;
        float3 coefficient = (sharedSums[0][index] + groundSkylight * sharedGround[0][index]) * solidAngle;
        // w of the first three: GroundSkyIrradiance (atmosphere_sampling.slang).
        irradianceOut[0].coefficients[index] = float4(coefficient, index < 3u ? groundSkyIrradiance[index] : 0.0);
    }
}
