#include "shader_helpers.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "volumetric_clouds.slang"
#include "cloud_reconstruction.slang"

// The volumetric clouds, one pixel of every 2 x 2 block of the scene per frame
// (VulkanAtmosphere::RecordClouds; cloud_reconstruction.slang). Per texel, for the block's pixel this
// frame marches: rgb the light the clouds and the haze in front of them send toward the camera,
// times CLOUD_TARGET_SCALE; a the transmittance through them. cloud_resolve.comp builds the full
// extent from it and the history.
// Set 1 binding 11 of the atmosphere's compute set.
[[vk::binding(11, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> cloudSamplesOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 texel = int2(dispatchThreadId.xy);
    if (any(texel >= ImageSize(cloudSamplesOut)))
    {
        return;
    }
    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    if (CloudsEnabled())
    {
        int2 extent = CloudSceneExtent();
        int2 pixel = min(texel * 2 + CloudMarchedPixelInBlock(), extent - 1);
        float2 uv = (float2(pixel) + 0.5) / float2(extent);
        float3 direction = ViewDirectionFromTexCoord(uv + CloudJitterUv());
        float4 clouds = CloudLayer(direction, SampleSkyForLighting(direction), CloudJitter(float2(pixel)), 32, 96);
        result = float4(min(clouds.rgb * CLOUD_TARGET_SCALE, float3(65504.0)), clouds.a);
    }
    cloudSamplesOut[texel] = result;
}
