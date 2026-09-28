// What the fragment shaders read from the environment: the sky (Hillaire 2020 sky-view LUT plus
// the sun disk), the HDRI, and aerial perspective. Include after scene_common.glsl.
#ifndef ATMOSPHERE_SAMPLING_GLSL
#define ATMOSPHERE_SAMPLING_GLSL

#include "atmosphere_common.glsl"
#include "ground_plane.glsl"
#include "brdf_common.glsl"
#include "spherical_harmonics.glsl"

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
// Set 0 bindings 8 and 9: the GGX-prefiltered sky (mip m for roughness m / 5) and the DFG table
// (x = N.V, y = roughness).
layout(set = 0, binding = 8) uniform samplerCube prefilteredEnvironment;
layout(set = 0, binding = 9) uniform sampler2D environmentBrdfLut;

#include "height_fog.glsl"

// The world direction through a full-screen texture coordinate (origin top left, as
// fullscreen.vert emits it), from the camera toward the far plane.
vec3 ViewDirectionFromTexCoord(vec2 texCoord)
{
    vec4 farPoint = ubo.invViewProj * vec4(texCoord * 2.0 - 1.0, DEPTH_FAR, 1.0);
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

// The sky-view LUT along a world direction: the light the atmosphere scatters toward the camera
// before the ray leaves it or meets the ground. The ground's own light is not in it.
vec3 SampleSkyViewLut(vec3 direction, out bool intersectGround)
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

    intersectGround = RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()) >= 0.0;
    vec2 uv = SkyViewLutParamsToUv(intersectGround, dot(direction, up), lightViewCos, viewHeight);
    return textureLod(atmosphereSkyViewLut, uv, 0.0).rgb;
}

// Where a ray from the camera meets the ground sphere: groundUp is the ground's normal there, and
// the result the atmosphere's transmittance between the camera and it. Both ends look back along
// -direction, which climbs away from the ground, so the transmittance LUT holds both halves:
// T(camera -> ground) = T(ground -> space) / T(camera -> space).
vec3 GroundViewTransmittance(vec3 direction, out vec3 groundUp)
{
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float t = max(RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()), 0.0);
    groundUp = normalize(camera + t * direction);
    float viewHeight = length(camera);
    vec3 fromGround = SampleTransmittance(atmosphereTransmittanceLut, BottomRadius() + PLANET_RADIUS_OFFSET_KM, dot(-direction, groundUp));
    vec3 fromCamera = SampleTransmittance(atmosphereTransmittanceLut, viewHeight, dot(-direction, camera / viewHeight));
    return clamp(fromGround / max(fromCamera, vec3(1e-6)), vec3(0.0), vec3(1.0));
}

// The sun's illuminance across its beam where the ground's normal is groundUp, through the
// atmosphere; zero once it has set there.
vec3 GroundSunIrradiance(vec3 groundUp)
{
    float cosSunZenith = dot(groundUp, ubo.sunDirectionAndMode.xyz);
    vec3 transmittance = SampleTransmittance(atmosphereTransmittanceLut, BottomRadius() + PLANET_RADIUS_OFFSET_KM, cosSunZenith);
    return cosSunZenith > 0.0 ? ubo.sunIlluminance.rgb * transmittance : vec3(0.0);
}

// The sky's illuminance on level ground, which atmosphere_irradiance.comp leaves in the w of the
// first three SH coefficients.
vec3 GroundSkyIrradiance()
{
    return vec3(skyIrradiance.coefficients[0].w, skyIrradiance.coefficients[1].w, skyIrradiance.coefficients[2].w);
}

// The share of the light the ground reflects specularly toward -direction (the split sum's DFG term).
float GroundSpecularAlbedo(vec3 direction, vec3 groundUp)
{
    const float size = 64.0;
    float NdV = max(dot(groundUp, -direction), 0.0);
    vec2 dfg = textureLod(environmentBrdfLut, clamp(vec2(NdV, GROUND_PLANE_ROUGHNESS), vec2(0.5 / size), vec2(1.0 - 0.5 / size)), 0.0).rg;
    return 0.04 * dfg.x + dfg.y;
}

// The atmosphere's radiance SH convolved with the clamped cosine around direction, over pi: the sky
// as a lobe as wide as a hemisphere sees it, standing in for the prefiltered environment at the
// ground's high roughness.
vec3 SkyRadianceAround(vec3 direction)
{
    float basis[9];
    EvaluateShBasis(direction, basis);
    vec3 irradiance = vec3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        irradiance += skyIrradiance.coefficients[i].rgb * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, vec3(0.0)) / ATMOSPHERE_PI;
}

// The ground's radiance toward the camera along direction, before the air in between, shaded as the
// lighting pass shades the ground plane (EvaluateBRDF and EvaluateSkyAmbient for its dielectric of
// GROUND_PLANE_ROUGHNESS), so the two meet at the far plane where this takes over: the sun through
// Burley's diffuse under the Fresnel toward it and the GGX lobe; the sky's illuminance diffused by
// what the split sum's specular share leaves, and mirroredRadiance reflected by that share.
// sunIrradiance is the sun's illuminance across its beam (N.L is applied here).
vec3 GroundSurfaceRadiance(vec3 direction, vec3 groundUp, vec3 sunIrradiance, vec3 skyIrradiance, vec3 mirroredRadiance)
{
    vec3 V = -direction;
    vec3 L = ubo.sunDirectionAndMode.xyz;
    vec3 H = normalize(V + L);
    float NdV = max(dot(groundUp, V), 1e-4);
    float NdL = max(dot(groundUp, L), 0.0);
    float NdH = max(dot(groundUp, H), 0.0);
    float VdH = max(dot(V, H), 0.0);
    float alpha = GROUND_PLANE_ROUGHNESS * GROUND_PLANE_ROUGHNESS;
    float alpha2 = alpha * alpha;
    float d = NdH * NdH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / (ATMOSPHERE_PI * d * d);
    float fresnel = 0.04 + 0.96 * pow(1.0 - VdH, 5.0);
    float sunSpecular = distribution * VisibilitySmithGgxCorrelated(NdV, NdL, alpha) * fresnel;
    float sunDiffuse = (1.0 - fresnel) * BurleyDiffuse(NdV, NdL, VdH, GROUND_PLANE_ROUGHNESS);
    vec3 sun = (ubo.groundAlbedo.rgb * sunDiffuse + sunSpecular) * sunIrradiance * NdL;

    float specularAlbedo = GroundSpecularAlbedo(direction, groundUp);
    vec3 sky = (1.0 - specularAlbedo) * ubo.groundAlbedo.rgb / ATMOSPHERE_PI * skyIrradiance + specularAlbedo * mirroredRadiance;
    return sun + sky;
}

// The ground below the horizon, which the sky-view LUT leaves out: a sphere lit by the transmitted
// sun and the sky, seen through the air between it and the camera (the LUT already holds that air's
// own in-scattering). atmosphere_irradiance.comp, which writes GroundSkyIrradiance, builds it from
// the parts instead.
vec3 GroundLuminance(vec3 direction)
{
    vec3 groundUp;
    vec3 viewTransmittance = GroundViewTransmittance(direction, groundUp);
    vec3 mirrored = SkyRadianceAround(reflect(direction, groundUp));
    return GroundSurfaceRadiance(direction, groundUp, GroundSunIrradiance(groundUp), GroundSkyIrradiance(), mirrored) * viewTransmittance;
}

// Sky luminance in cd/m^2 seen from the camera along a world direction.
vec3 SampleSky(vec3 direction)
{
    bool intersectGround;
    vec3 luminance = SampleSkyViewLut(direction, intersectGround);
    if (intersectGround)
    {
        return luminance + GroundLuminance(direction);
    }
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    return luminance + SunDisk(direction, camera / viewHeight, viewHeight);
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
// camera are untouched. The height fog goes on top, as in Unreal; both are of the form
// color * T + L, which gi_composite.frag relies on. Off unless the environment is the atmosphere.
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
    return ApplyHeightFog(color * transmittance + aerialPerspective.rgb * weight, worldPosition);
}

// Radiance that lights surfaces: the sky as SampleSky shows it, without the sun's disk (the sun
// is a light of its own).
vec3 SampleSkyForLighting(vec3 direction)
{
    bool intersectGround;
    vec3 luminance = SampleSkyViewLut(direction, intersectGround);
    return intersectGround ? luminance + GroundLuminance(direction) : luminance;
}

#endif
