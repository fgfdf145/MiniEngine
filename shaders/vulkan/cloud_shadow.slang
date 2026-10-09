// Cloud shadows (docs/design/2026-09-28-cloud-shadows-design.md): the volumetric clouds'
// transmittance toward the sun, stored per point of the ground in a map centred on the camera
// (cloud_shadow.comp writes it every frame), and looked up by every surface lit by the shadow
// casting sun. The C++ mirror is CloudShadowUv in engine/renderer/volumetric_clouds.cpp. Include
// after atmosphere_common.slang.
#ifndef CLOUD_SHADOW_SLANG
#define CLOUD_SHADOW_SLANG

// Set 0 binding 26: the map, r the transmittance (VulkanAtmosphere), linear clamped.
[[vk::binding(26, 0)]] Texture2D cloudShadowMap : register(t26, space0);
[[vk::binding(FRAME_SAMPLER(26), 0)]] SamplerState cloudShadowMapSampler : register(s90, space0);

// Must match kCloudShadowMapSize, kCloudShadowExtentMeters, kCloudShadowMinSunHeight and
// kCloudShadowEdgeFade in engine/renderer/volumetric_clouds.h.
static const float CLOUD_SHADOW_MAP_SIZE = 512.0;
static const float CLOUD_SHADOW_EXTENT_M = 16000.0;
static const float CLOUD_SHADOW_MIN_SUN_HEIGHT = 0.05;
static const float CLOUD_SHADOW_EDGE_FADE = 0.1;

bool CloudShadowsEnabled()
{
    return EnvironmentMode() == ENVIRONMENT_ATMOSPHERE && ubo.cloudLayer.w > 0.0;
}

// The map's centre in world x, z (metres): the camera snapped to whole texels, so the map slides
// under a moving camera without its texels shimmering.
float2 CloudShadowMapCenter()
{
    float texel = CLOUD_SHADOW_EXTENT_M / CLOUD_SHADOW_MAP_SIZE;
    return floor(ubo.cameraWorldPosition.xz / texel) * texel;
}

// Where the ray from worldPos toward the sun crosses world y = 0, as the map's uv. The ray and the
// map texel's own ray toward the sun are the same line, so they cross the same clouds.
float2 CloudShadowUv(float3 worldPos)
{
    float3 sun = ubo.sunDirectionAndMode.xyz;
    float2 ground = worldPos.xz - sun.xz * (worldPos.y / max(sun.y, CLOUD_SHADOW_MIN_SUN_HEIGHT));
    return (ground - CloudShadowMapCenter()) / CLOUD_SHADOW_EXTENT_M + 0.5;
}

// The share of the sun the clouds let through to worldPos: 1 without clouds, fading to 1 over the
// map's outer tenth so its edge never shows.
float CloudShadow(float3 worldPos)
{
    if (!CloudShadowsEnabled())
    {
        return 1.0;
    }
    float2 uv = CloudShadowUv(worldPos);
    float2 edge = min(uv, 1.0 - uv);
    float inside = clamp(min(edge.x, edge.y) / CLOUD_SHADOW_EDGE_FADE, 0.0, 1.0);
    if (inside <= 0.0)
    {
        return 1.0;
    }
    return lerp(1.0, cloudShadowMap.SampleLevel(cloudShadowMapSampler, uv, 0.0).r, inside);
}

#endif
