#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "volumetric_clouds.slang"
#include "cubemap_common.slang"

// The sky's radiance into mip 0 of the radiance cube: 2 x 2 samples per texel, no sun disk, with
// the volumetric clouds in front of it.
[[vk::binding(0, 1)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> radianceOut : register(u0, space1);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint3 id = dispatchThreadId;
    if (id.x >= uint(RADIANCE_CUBE_SIZE) || id.y >= uint(RADIANCE_CUBE_SIZE))
    {
        return;
    }
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    float3 sum = float3(0.0);
    for (uint sy = 0u; sy < 2u; ++sy)
    {
        for (uint sx = 0u; sx < 2u; ++sx)
        {
            float2 uv = (float2(id.xy) + (float2(sx, sy) + 0.5) * 0.5) / RADIANCE_CUBE_SIZE;
            float3 direction = CubeFaceDirection(id.z, uv);
            sum += hdri ? SampleEnvironmentMap(direction) : SampleSkyForLighting(direction);
        }
    }
    float3 radiance = sum * 0.25;
    if (!hdri)
    {
        // The clouds too, so reflections show them: one march through the texel's centre, with
        // fewer steps than the sky's, as the capture is blurred and all of it runs at once.
        float3 center = CubeFaceDirection(id.z, (float2(id.xy) + 0.5) / RADIANCE_CUBE_SIZE);
        radiance = ApplyClouds(radiance, radiance, center, 0.5, 12, 32);
    }
    radianceOut[int3(id)] = float4(min(radiance, float3(65504.0)), 1.0);
}
