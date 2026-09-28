// Cloud shadows (docs/design/2026-09-28-cloud-shadows-design.md): the volumetric clouds'
// transmittance toward the sun, stored per point of the ground in a map centred on the camera
// (cloud_shadow.comp writes it every frame), and looked up by every surface lit by the shadow
// casting sun. The C++ mirror is CloudShadowUv in engine/renderer/volumetric_clouds.cpp. Include
// after atmosphere_common.glsl.
#ifndef CLOUD_SHADOW_GLSL
#define CLOUD_SHADOW_GLSL

// Set 0 binding 26: the map, r the transmittance (VulkanAtmosphere), linear clamped.
layout(set = 0, binding = 26) uniform sampler2D cloudShadowMap;

// Must match kCloudShadowMapSize, kCloudShadowExtentMeters, kCloudShadowMinSunHeight and
// kCloudShadowEdgeFade in engine/renderer/volumetric_clouds.h.
const float CLOUD_SHADOW_MAP_SIZE = 512.0;
const float CLOUD_SHADOW_EXTENT_M = 16000.0;
const float CLOUD_SHADOW_MIN_SUN_HEIGHT = 0.05;
const float CLOUD_SHADOW_EDGE_FADE = 0.1;

bool CloudShadowsEnabled()
{
    return EnvironmentMode() == ENVIRONMENT_ATMOSPHERE && ubo.cloudLayer.w > 0.0;
}

// The map's centre in world x, z (metres): the camera snapped to whole texels, so the map slides
// under a moving camera without its texels shimmering.
vec2 CloudShadowMapCenter()
{
    float texel = CLOUD_SHADOW_EXTENT_M / CLOUD_SHADOW_MAP_SIZE;
    return floor(ubo.cameraWorldPosition.xz / texel) * texel;
}

// Where the ray from worldPos toward the sun crosses world y = 0, as the map's uv. The ray and the
// map texel's own ray toward the sun are the same line, so they cross the same clouds.
vec2 CloudShadowUv(vec3 worldPos)
{
    vec3 sun = ubo.sunDirectionAndMode.xyz;
    vec2 ground = worldPos.xz - sun.xz * (worldPos.y / max(sun.y, CLOUD_SHADOW_MIN_SUN_HEIGHT));
    return (ground - CloudShadowMapCenter()) / CLOUD_SHADOW_EXTENT_M + 0.5;
}

// The share of the sun the clouds let through to worldPos: 1 without clouds, fading to 1 over the
// map's outer tenth so its edge never shows.
float CloudShadow(vec3 worldPos)
{
    if (!CloudShadowsEnabled())
    {
        return 1.0;
    }
    vec2 uv = CloudShadowUv(worldPos);
    vec2 edge = min(uv, 1.0 - uv);
    float inside = clamp(min(edge.x, edge.y) / CLOUD_SHADOW_EDGE_FADE, 0.0, 1.0);
    if (inside <= 0.0)
    {
        return 1.0;
    }
    return mix(1.0, textureLod(cloudShadowMap, uv, 0.0).r, inside);
}

#endif
