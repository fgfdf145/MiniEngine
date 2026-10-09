// What the fragment shaders read from the environment: the sky (Hillaire 2020 sky-view LUT plus
// the sun disk), the HDRI, and aerial perspective. Include after scene_common.slang.
#ifndef ATMOSPHERE_SAMPLING_SLANG
#define ATMOSPHERE_SAMPLING_SLANG

#include "atmosphere_common.slang"
#include "ground_plane.slang"
#include "brdf_common.slang"
#include "spherical_harmonics.slang"

// Set 0 bindings 3 to 6; see VulkanFrameDescriptorSetLayout.
[[vk::binding(3, 0)]] Texture2D atmosphereTransmittanceLut;
[[vk::binding(FRAME_SAMPLER(3), 0)]] SamplerState atmosphereTransmittanceLutSampler;
[[vk::binding(4, 0)]] Texture2D atmosphereSkyViewLut;
[[vk::binding(FRAME_SAMPLER(4), 0)]] SamplerState atmosphereSkyViewLutSampler;
[[vk::binding(5, 0)]] Texture3D atmosphereAerialPerspective;
[[vk::binding(FRAME_SAMPLER(5), 0)]] SamplerState atmosphereAerialPerspectiveSampler;
[[vk::binding(6, 0)]] Texture2D environmentMap;
[[vk::binding(FRAME_SAMPLER(6), 0)]] SamplerState environmentMapSampler;

// The transmittance LUT, for the atmosphere functions that take an IAtmosphereLut.
SeparateAtmosphereLut FrameTransmittanceLut()
{
    return AtmosphereLut(atmosphereTransmittanceLut, atmosphereTransmittanceLutSampler);
}
// Set 0 binding 7: the atmosphere's radiance SH, written by atmosphere_irradiance.comp.
struct SkyIrradiance
{
    float4 coefficients[9];
};
[[vk::binding(7, 0)]] StructuredBuffer<SkyIrradiance> skyIrradiance; // one element
// Set 0 bindings 8 and 9: the GGX-prefiltered sky (mip m for roughness m / 5) and the DFG table
// (x = N.V, y = roughness).
[[vk::binding(8, 0)]] TextureCube prefilteredEnvironment;
[[vk::binding(FRAME_SAMPLER(8), 0)]] SamplerState prefilteredEnvironmentSampler;
[[vk::binding(9, 0)]] Texture2D environmentBrdfLut;
[[vk::binding(FRAME_SAMPLER(9), 0)]] SamplerState environmentBrdfLutSampler;

#include "height_fog.slang"
#include "cloud_shadow.slang"

// The world direction through a full-screen texture coordinate (origin top left, as
// fullscreen.vert emits it), from the camera toward the far plane.
float3 ViewDirectionFromTexCoord(float2 texCoord)
{
    float4 farPoint = mul(ubo.invViewProj, float4(texCoord * 2.0 - 1.0, DEPTH_FAR, 1.0));
    return normalize(farPoint.xyz / farPoint.w - ubo.cameraWorldPosition.xyz);
}

// The sun's disk, if direction falls inside it: its illuminance spread over its solid angle,
// dimmed by the atmosphere along the view ray and darkened toward the limb.
float3 SunDisk(float3 direction, float3 up, float viewHeight)
{
    float3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float cosRadius = ubo.sunIlluminance.w;
    if (dot(direction, sunDirection) < cosRadius)
    {
        return float3(0.0);
    }
    float3 transmittance = SampleTransmittance(FrameTransmittanceLut(), viewHeight, dot(direction, up));
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
// before the ray leaves it or meets the ground, and above the horizon the night sky's own glow
// (airglow and starlight), which the sun's and moon's light swamps until night. The ground's own
// light is not in it.
float3 SampleSkyViewLut(float3 direction, out bool intersectGround)
{
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    float3 up = camera / viewHeight;

    float3 sunDirection = ubo.sunDirectionAndMode.xyz;
    float lightViewCos = 1.0;
    float3 side = cross(up, direction);
    if (dot(side, side) > 1e-10)
    {
        side = normalize(side);
        float3 forward = normalize(cross(side, up));
        float2 sunOnPlane = float2(dot(sunDirection, forward), dot(sunDirection, side));
        float length2 = dot(sunOnPlane, sunOnPlane);
        lightViewCos = length2 > 1e-12 ? sunOnPlane.x * rsqrt(length2) : 1.0;
    }

    intersectGround = RaySphereIntersectNearest(camera, direction, float3(0.0), BottomRadius()) >= 0.0;
    float2 uv = SkyViewLutParamsToUv(intersectGround, dot(direction, up), lightViewCos, viewHeight);
    float3 luminance = atmosphereSkyViewLut.SampleLevel(atmosphereSkyViewLutSampler, uv, 0.0).rgb;
    return intersectGround ? luminance : luminance + ubo.nightSky.rgb;
}

// The direction a seamless horizon reads the sky along for a ray that would meet the ground: its
// mirror image across the horizon, in the plane through the ray and the vertical. The zenith angle
// theta becomes 2 theta_h - theta, where theta_h is the horizon's own (sin theta_h = R / H, past 90
// degrees for a camera above the ground), so the two agree where the ground begins.
float3 FoldAcrossHorizon(float3 direction)
{
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    float3 up = camera / viewHeight;
    float cosZenith = clamp(dot(direction, up), -1.0, 1.0);
    float sinZenith = sqrt(1.0 - cosZenith * cosZenith);

    float sinHorizon = min(BottomRadius() / viewHeight, 1.0);
    float cosHorizon = -sqrt(max(0.0, 1.0 - sinHorizon * sinHorizon));
    float cosDoubleHorizon = 1.0 - 2.0 * sinHorizon * sinHorizon;
    float sinDoubleHorizon = 2.0 * sinHorizon * cosHorizon;
    float cosFolded = cosDoubleHorizon * cosZenith + sinDoubleHorizon * sinZenith;
    float sinFolded = sinDoubleHorizon * cosZenith - cosDoubleHorizon * sinZenith;

    float3 across = direction - cosZenith * up;
    float acrossLength = length(across);
    // Straight down has no azimuth; any will do.
    across = acrossLength > 1e-5 ? across / acrossLength : normalize(cross(up, abs(up.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
    return normalize(cosFolded * up + sinFolded * across);
}

// The sky along a ray that would meet the ground, with the ground taken away (FoldAcrossHorizon).
float3 SampleFoldedSkyViewLut(float3 direction)
{
    bool intersectGround;
    return SampleSkyViewLut(FoldAcrossHorizon(direction), intersectGround);
}

// Where a ray from the camera meets the ground sphere: groundUp is the ground's normal there, and
// the result the atmosphere's transmittance between the camera and it. Both ends look back along
// -direction, which climbs away from the ground, so the transmittance LUT holds both halves:
// T(camera -> ground) = T(ground -> space) / T(camera -> space).
float3 GroundViewTransmittance(float3 direction, out float3 groundUp)
{
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float t = max(RaySphereIntersectNearest(camera, direction, float3(0.0), BottomRadius()), 0.0);
    groundUp = normalize(camera + t * direction);
    float viewHeight = length(camera);
    float3 fromGround = SampleTransmittance(FrameTransmittanceLut(), BottomRadius() + PLANET_RADIUS_OFFSET_KM, dot(-direction, groundUp));
    float3 fromCamera = SampleTransmittance(FrameTransmittanceLut(), viewHeight, dot(-direction, camera / viewHeight));
    return clamp(fromGround / max(fromCamera, float3(1e-6)), float3(0.0), float3(1.0));
}

// The sun's illuminance across its beam where the ground's normal is groundUp, through the
// atmosphere; zero once it has set there.
float3 GroundSunIrradiance(float3 groundUp)
{
    float cosSunZenith = dot(groundUp, ubo.sunDirectionAndMode.xyz);
    float3 transmittance = SampleTransmittance(FrameTransmittanceLut(), BottomRadius() + PLANET_RADIUS_OFFSET_KM, cosSunZenith);
    return cosSunZenith > 0.0 ? ubo.sunIlluminance.rgb * transmittance : float3(0.0);
}

// The sky's illuminance on level ground, which atmosphere_irradiance.comp leaves in the w of the
// first three SH coefficients.
float3 GroundSkyIrradiance()
{
    return float3(skyIrradiance[0].coefficients[0].w, skyIrradiance[0].coefficients[1].w, skyIrradiance[0].coefficients[2].w);
}

// The share of the light the ground reflects specularly toward -direction (the split sum's DFG term).
float GroundSpecularAlbedo(float3 direction, float3 groundUp)
{
    const float size = 64.0;
    float NdV = max(dot(groundUp, -direction), 0.0);
    float2 dfg = environmentBrdfLut.SampleLevel(environmentBrdfLutSampler, clamp(float2(NdV, GROUND_PLANE_ROUGHNESS), float2(0.5 / size), float2(1.0 - 0.5 / size)), 0.0).rg;
    return 0.04 * dfg.x + dfg.y;
}

// The atmosphere's radiance SH convolved with the clamped cosine around direction, over pi: the sky
// as a lobe as wide as a hemisphere sees it, standing in for the prefiltered environment at the
// ground's high roughness.
float3 SkyRadianceAround(float3 direction)
{
    float basis[9];
    EvaluateShBasis(direction, basis);
    float3 irradiance = float3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        irradiance += skyIrradiance[0].coefficients[i].rgb * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, float3(0.0)) / ATMOSPHERE_PI;
}

// The ground's radiance toward the camera along direction, before the air in between, shaded as the
// lighting pass shades the ground plane (EvaluateBRDF and EvaluateSkyAmbient for its dielectric of
// GROUND_PLANE_ROUGHNESS), so the two meet at the far plane where this takes over: the sun through
// Burley's diffuse under the Fresnel toward it and the GGX lobe; the sky's illuminance diffused by
// what the split sum's specular share leaves, and mirroredRadiance reflected by that share.
// sunIrradiance is the sun's illuminance across its beam (N.L is applied here).
float3 GroundSurfaceRadiance(float3 direction, float3 groundUp, float3 sunIrradiance, float3 skyIrradiance, float3 mirroredRadiance)
{
    float3 V = -direction;
    float3 L = ubo.sunDirectionAndMode.xyz;
    float3 H = normalize(V + L);
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
    float3 sun = (ubo.groundAlbedo.rgb * sunDiffuse + sunSpecular) * sunIrradiance * NdL;

    float specularAlbedo = GroundSpecularAlbedo(direction, groundUp);
    float3 sky = (1.0 - specularAlbedo) * ubo.groundAlbedo.rgb / ATMOSPHERE_PI * skyIrradiance + specularAlbedo * mirroredRadiance;
    return sun + sky;
}

// The ground below the horizon, which the sky-view LUT leaves out: a sphere lit by the transmitted
// sun and the sky, seen through the air between it and the camera (the LUT already holds that air's
// own in-scattering). atmosphere_irradiance.comp, which writes GroundSkyIrradiance, builds it from
// the parts instead.
float3 GroundLuminance(float3 direction)
{
    float3 groundUp;
    float3 viewTransmittance = GroundViewTransmittance(direction, groundUp);
    float3 mirrored = SkyRadianceAround(reflect(direction, groundUp));
    return GroundSurfaceRadiance(direction, groundUp, GroundSunIrradiance(groundUp), GroundSkyIrradiance(), mirrored) * viewTransmittance;
}

// Sky luminance in cd/m^2 seen from the camera along a world direction.
float3 SampleSky(float3 direction)
{
    bool intersectGround;
    float3 luminance = SampleSkyViewLut(direction, intersectGround);
    if (intersectGround)
    {
        return SeamlessHorizon() ? SampleFoldedSkyViewLut(direction) : luminance + GroundLuminance(direction);
    }
    float3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    return luminance + SunDisk(direction, camera / viewHeight, viewHeight);
}

// The equirectangular HDRI along a world direction: u = 0.5 at -Z, the default camera's view,
// growing toward +X; v = 0 straight up. hdriParameters: x intensity, y rotation in turns.
float3 SampleEnvironmentMap(float3 direction)
{
    float u = 0.5 + atan2(direction.x, -direction.z) / (2.0 * ATMOSPHERE_PI) + ubo.hdriParameters.y;
    float v = acos(clamp(direction.y, -1.0, 1.0)) / ATMOSPHERE_PI;
    return environmentMap.SampleLevel(environmentMapSampler, float2(u, v), 0.0).rgb * ubo.hdriParameters.x;
}

// Hazes a surface's radiance by the atmosphere between it and the camera: color * T + L, from the
// aerial perspective volume at the surface's screen position and view distance (times the
// scene's distance scale). The first half slice fades in from nothing, so surfaces right at the
// camera are untouched. The height fog goes on top, as in Unreal; both are of the form
// color * T + L, which gi_composite.frag relies on. Off unless the environment is the atmosphere.
float3 ApplyAerialPerspective(float3 color, float3 worldPosition)
{
    if (EnvironmentMode() != ENVIRONMENT_ATMOSPHERE)
    {
        return color;
    }
    float4 clip = mul(mul(ubo.proj, ubo.view), float4(worldPosition, 1.0));
    float2 uv = clamp(clip.xy / clip.w * 0.5 + 0.5, float2(0.0), float2(1.0));
    float distanceKm = length(worldPosition - ubo.cameraWorldPosition.xyz) * 0.001 * ubo.atmosphereRadii.z;
    float slice = distanceKm / AERIAL_PERSPECTIVE_KM_PER_SLICE;
    float weight = 1.0;
    if (slice < 0.5)
    {
        weight = clamp(slice * 2.0, 0.0, 1.0);
        slice = 0.5;
    }
    float w = sqrt(slice / AERIAL_PERSPECTIVE_SLICE_COUNT);
    float4 aerialPerspective = atmosphereAerialPerspective.SampleLevel(atmosphereAerialPerspectiveSampler, float3(uv, w), 0.0);
    float transmittance = 1.0 - weight * (1.0 - aerialPerspective.a);
    return ApplyHeightFog(color * transmittance + aerialPerspective.rgb * weight, worldPosition);
}

// Radiance that lights surfaces: the sky as SampleSky shows it, without the sun's disk (the sun
// is a light of its own).
float3 SampleSkyForLighting(float3 direction)
{
    bool intersectGround;
    float3 luminance = SampleSkyViewLut(direction, intersectGround);
    if (!intersectGround)
    {
        return luminance;
    }
    return SeamlessHorizon() ? SampleFoldedSkyViewLut(direction) : luminance + GroundLuminance(direction);
}

#endif
