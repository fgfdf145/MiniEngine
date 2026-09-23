// What the fragment shaders read from the environment: the sky (Hillaire 2020 sky-view LUT plus
// the sun disk), the HDRI, and aerial perspective. Include after scene_common.glsl.
#ifndef ATMOSPHERE_SAMPLING_GLSL
#define ATMOSPHERE_SAMPLING_GLSL

#include "atmosphere_common.glsl"

// Set 0 bindings 3 to 6; see VulkanFrameDescriptorSetLayout.
layout(set = 0, binding = 3) uniform sampler2D atmosphereTransmittanceLut;
layout(set = 0, binding = 4) uniform sampler2D atmosphereSkyViewLut;
layout(set = 0, binding = 5) uniform sampler3D atmosphereAerialPerspective;
layout(set = 0, binding = 6) uniform sampler2D environmentMap;
// Set 0 binding 7: the atmosphere's radiance SH, written by atmosphere_irradiance.comp.
layout(set = 0, binding = 7, std430) readonly buffer SkyIrradiance
{
    vec4 coefficients[9];
}
skyIrradiance;

// The world direction through a full-screen texture coordinate (origin top left, as
// fullscreen.vert emits it), from the camera toward the far plane.
vec3 ViewDirectionFromTexCoord(vec2 texCoord)
{
    vec4 farPoint = ubo.invViewProj * vec4(texCoord * 2.0 - 1.0, 1.0, 1.0);
    return normalize(farPoint.xyz / farPoint.w - ubo.cameraWorldPosition.xyz);
}

// The sun's disk, if direction falls inside it: its illuminance spread over its solid angle,
// dimmed by the atmosphere along the view ray and darkened toward the limb.
vec3 SunDisk(vec3 direction, vec3 up, float viewHeight)
{
    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float cosRadius = ubo.sunIlluminance.w;
    if (dot(direction, sunDirection) < cosRadius)
    {
        return vec3(0.0);
    }
    vec3 transmittance = SampleTransmittance(atmosphereTransmittanceLut, viewHeight, dot(direction, up));
    float solidAngle = 2.0 * ATMOSPHERE_PI * (1.0 - cosRadius);
    // asin of the cross product's length stays precise at the tiny angles inside the disk, where
    // acos of the dot product does not.
    float angle = asin(min(length(cross(direction, sunDirection)), 1.0));
    float radius = acos(cosRadius);
    float centerToEdge = clamp(angle / radius, 0.0, 1.0);
    float mu = sqrt(max(0.0, 1.0 - centerToEdge * centerToEdge));
    float limbDarkening = 1.0 - 0.6 * (1.0 - sqrt(mu));
    return ubo.sunIlluminance.rgb / solidAngle * transmittance * limbDarkening;
}

// Sky luminance in cd/m^2 seen from the camera along a world direction.
vec3 SampleSky(vec3 direction)
{
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    float viewZenithCos = dot(direction, up);

    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float lightViewCos = 1.0;
    vec3 side = cross(up, direction);
    if (dot(side, side) > 1e-10)
    {
        side = normalize(side);
        vec3 forward = normalize(cross(side, up));
        vec2 sunOnPlane = vec2(dot(sunDirection, forward), dot(sunDirection, side));
        float length2 = dot(sunOnPlane, sunOnPlane);
        lightViewCos = length2 > 1e-12 ? sunOnPlane.x * inversesqrt(length2) : 1.0;
    }

    bool intersectGround = RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()) >= 0.0;
    vec2 uv = SkyViewLutParamsToUv(intersectGround, viewZenithCos, lightViewCos, viewHeight);
    vec3 luminance = textureLod(atmosphereSkyViewLut, uv, 0.0).rgb;
    if (!intersectGround)
    {
        luminance += SunDisk(direction, up, viewHeight);
    }
    return luminance;
}

// The equirectangular HDRI along a world direction: u = 0.5 at -Z, the default camera's view,
// growing toward +X; v = 0 straight up. hdriParameters: x intensity, y rotation in turns.
vec3 SampleEnvironmentMap(vec3 direction)
{
    float u = 0.5 + atan(direction.x, -direction.z) / (2.0 * ATMOSPHERE_PI) + ubo.hdriParameters.y;
    float v = acos(clamp(direction.y, -1.0, 1.0)) / ATMOSPHERE_PI;
    return textureLod(environmentMap, vec2(u, v), 0.0).rgb * ubo.hdriParameters.x;
}

// Hazes a surface's radiance by the atmosphere between it and the camera: color * T + L, from the
// aerial perspective volume at the surface's screen position and view distance (times the
// scene's distance scale). The first half slice fades in from nothing, so surfaces right at the
// camera are untouched. Off unless the environment is the atmosphere.
vec3 ApplyAerialPerspective(vec3 color, vec3 worldPosition)
{
    if (EnvironmentMode() != ENVIRONMENT_ATMOSPHERE)
    {
        return color;
    }
    vec4 clip = ubo.proj * ubo.view * vec4(worldPosition, 1.0);
    vec2 uv = clamp(clip.xy / clip.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    float distanceKm = length(worldPosition - ubo.cameraWorldPosition.xyz) * 0.001 * ubo.atmosphereRadii.z;
    float slice = distanceKm / AERIAL_PERSPECTIVE_KM_PER_SLICE;
    float weight = 1.0;
    if (slice < 0.5)
    {
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    float w = sqrt(slice / AERIAL_PERSPECTIVE_SLICE_COUNT);
    vec4 aerialPerspective = textureLod(atmosphereAerialPerspective, vec3(uv, w), 0.0);
    float transmittance = 1.0 - weight * (1.0 - aerialPerspective.a);
    return color * transmittance + aerialPerspective.rgb * weight;
}

// Radiance that lights surfaces: the sky without the sun's disk (the sun is a light of its own),
// and below the horizon the ground lit by the transmitted sun, which the sky-view LUT leaves out.
vec3 SampleSkyForLighting(vec3 direction)
{
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;

    float lightViewCos = 1.0;
    vec3 side = cross(up, direction);
    if (dot(side, side) > 1e-10)
    {
        side = normalize(side);
        vec3 forward = normalize(cross(side, up));
        vec2 sunOnPlane = vec2(dot(sunDirection, forward), dot(sunDirection, side));
        float length2 = dot(sunOnPlane, sunOnPlane);
        lightViewCos = length2 > 1e-12 ? sunOnPlane.x * inversesqrt(length2) : 1.0;
    }
    bool intersectGround = RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()) >= 0.0;
    vec2 uv = SkyViewLutParamsToUv(intersectGround, dot(direction, up), lightViewCos, viewHeight);
    vec3 luminance = textureLod(atmosphereSkyViewLut, uv, 0.0).rgb;
    if (intersectGround)
    {
        float cosSunZenith = dot(up, sunDirection);
        vec3 transmittance = SampleTransmittance(atmosphereTransmittanceLut, BottomRadius() + PLANET_RADIUS_OFFSET_KM, cosSunZenith);
        luminance += ubo.groundAlbedo.rgb / ATMOSPHERE_PI * ubo.sunIlluminance.rgb * transmittance * max(cosSunZenith, 0.0);
    }
    return luminance;
}

#endif
