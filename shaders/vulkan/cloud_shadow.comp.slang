#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "volumetric_clouds.slang"

// The cloud shadow map (cloud_shadow.slang), every frame: per texel, the clouds' transmittance along
// the ray from its point of the ground (world y = 0) toward the sun, through the base shape without
// the detail, which a texel of 31 m could not hold anyway.
// Set 1 binding 9 of the atmosphere's compute set.
[[vk::binding(9, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> cloudShadowOut : register(u9, space1);

static const int CLOUD_SHADOW_STEPS = 16;
// Far enough for the sun 3 degrees up to cross a 10 km layer.
static const float CLOUD_SHADOW_MAX_DISTANCE_KM = 200.0;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 texel = int2(dispatchThreadId.xy);
    if (any(texel >= int2(CLOUD_SHADOW_MAP_SIZE)))
    {
        return;
    }
    float3 sun = ubo.sunDirectionAndMode.xyz;
    float transmittance = 1.0;
    if (CloudShadowsEnabled() && sun.y > 0.0)
    {
        float2 groundMeters = CloudShadowMapCenter() + ((float2(texel) + 0.5) / CLOUD_SHADOW_MAP_SIZE - 0.5) * CLOUD_SHADOW_EXTENT_M;
        // World metres to the atmosphere's planet-centred kilometres, as ToAtmosphereCameraPositionKm.
        float3 origin = float3(groundMeters.x * 0.001, BottomRadius(), groundMeters.y * 0.001);
        float inner = BottomRadius() + ubo.cloudLayer.x;
        float thickness = ubo.cloudLayer.y;
        // The planet a kilometre down, so the ray leaving the ground is not stopped by it.
        float2 span = CloudShellInterval(origin, sun, BottomRadius() - 1.0, inner, inner + thickness, CLOUD_SHADOW_MAX_DISTANCE_KM);
        if (span.y > span.x)
        {
            float dt = (span.y - span.x) / float(CLOUD_SHADOW_STEPS);
            float opticalDepth = 0.0;
            for (int step = 0; step < CLOUD_SHADOW_STEPS; ++step)
            {
                float3 p = origin + sun * (span.x + (float(step) + 0.5) * dt);
                opticalDepth += CloudExtinction(p, (length(p) - inner) / thickness, 0.0) * dt;
            }
            transmittance = exp(-opticalDepth);
        }
    }
    cloudShadowOut[texel] = float4(transmittance, 0.0, 0.0, 1.0);
}
