#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

// SceneLightData, the LIGHT_* and SHADOW_CASCADE_COUNT constants and the ubo block.
#include "scene_common.glsl"
// The sky's SH irradiance for the ambient term under a physical sky.
#include "atmosphere_sampling.glsl"
#include "spherical_harmonics.glsl"
#include "cubemap_common.glsl"

// One layer per cascade, sampled with a LESS_OR_EQUAL depth comparison (see VulkanShadowPass).
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;

// Every light the shader evaluates: ubo.lightCounts.x directional lights, then the local ones. The
// C++ side is GpuLightData, written by VulkanUniformBuffer::Update.
layout(set = 0, binding = 10, std430) readonly buffer SceneLightBuffer
{
    SceneLightData lights[];
}
sceneLights;

// The local lights each cluster may be lit by (LightClusterGrid in engine/renderer/light_clusters.h):
// ranges[cluster] = (offset, count) into indices, whose entries index sceneLights.lights.
layout(set = 0, binding = 11, std430) readonly buffer LightClusterBuffer
{
    uvec2 ranges[LIGHT_CLUSTER_COUNT];
    uint indices[];
}
lightClusters;

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR microfacet BRDF helpers
// ---------------------------------------------------------------------------

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = NdH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / max(NdV * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdV, roughness) * GeometrySchlickGGX(NdL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF contribution for one light sample
// Returns outgoing radiance.
// ---------------------------------------------------------------------------
vec3 EvaluateBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness,
    vec3 energyCompensation,
    vec3 radiance)
{
    float NdL = max(dot(N, L), 0.0);
    if (NdL <= 0.0)
        return vec3(0.0);

    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    float HdV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(HdV, F0);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);

    vec3 specular = (D * G * F) / max(4.0 * NdV * NdL, 0.0001) * energyCompensation;

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * radiance * NdL;
}

// ---------------------------------------------------------------------------
// Clearcoat (KHR_materials_clearcoat)
// ---------------------------------------------------------------------------

// The clear dielectric layer over the base, as the Khronos glTF sample viewer shades it: a second
// GGX lobe with F0 = 0.04 (IOR 1.5), its own roughness and normal, weighted by factor. A factor of
// 0 is no coat, and every coat term below is skipped for it.
struct CoatParams
{
    float factor;
    // Perceptual roughness, already floored at 0.04 like the base's.
    float roughness;
    // The geometric normal: the coat is not normal mapped (no clearcoatNormalTexture support).
    vec3 normal;
};

CoatParams NoCoat()
{
    CoatParams coat;
    coat.factor = 0.0;
    coat.roughness = 1.0;
    coat.normal = vec3(0.0, 0.0, 1.0);
    return coat;
}

const vec3 COAT_F0 = vec3(0.04);

// The coat lobe's outgoing radiance for light arriving along L. Radiance is the light's
// illuminance at normal incidence, as EvaluateBRDF takes it.
vec3 EvaluateCoatSpecular(CoatParams coat, vec3 V, vec3 L, vec3 radiance)
{
    float NdL = max(dot(coat.normal, L), 0.0);
    if (NdL <= 0.0)
        return vec3(0.0);

    vec3 H = normalize(V + L);
    float NdV = max(dot(coat.normal, V), 0.0);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), COAT_F0);
    float D = DistributionGGX(coat.normal, H, coat.roughness);
    float G = GeometrySmith(coat.normal, V, L, coat.roughness);
    return (D * G * F) / max(4.0 * NdV * NdL, 0.0001) * radiance * NdL;
}

// ---------------------------------------------------------------------------
// Distance attenuation: inverse square, windowed to zero at the light's range
// ---------------------------------------------------------------------------
// The windowing half on its own: 1 near the light, falling smoothly to 0 at the range.
float RangeWindow(float distance, float range)
{
    float ratio = distance / max(range, 0.001);
    float ratio4 = ratio * ratio * ratio * ratio;
    float num = clamp(1.0 - ratio4, 0.0, 1.0);
    return num * num;
}

// Physical 1 / d^2. The floor stops the singularity at the light's position: 1 cm, far below any
// distance a lit surface sits at. The "+ 1" some engines put in the denominator instead halves the
// light at one metre and cuts it to a tenth at thirty centimetres, which does not hold up once
// intensities are in lumens.
const float kMinLightDistanceSquared = 1e-4;

float SmoothDistanceAttenuation(float distance, float range)
{
    return RangeWindow(distance, range) / max(distance * distance, kMinLightDistanceSquared);
}

// ---------------------------------------------------------------------------
// Rectangular area light
// ---------------------------------------------------------------------------

// The cosine-weighted solid angle of a polygon, the integral of dot(N, w) over the directions w
// it covers, in Lambert's closed form: one term per edge. Corners must wind counterclockwise
// about the light's emitting normal. Polygons crossing the receiver's horizon are not clipped,
// which overestimates slightly there; the clamp only keeps the result from going negative.
float RectangleFormFactor(vec3 worldPos, vec3 N, vec3 corners[4])
{
    float sum = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec3 a = normalize(corners[i] - worldPos);
        vec3 b = normalize(corners[(i + 1) % 4] - worldPos);
        vec3 edgeNormal = cross(b, a);
        float sinAngle = length(edgeNormal);
        if (sinAngle > 1e-7)
        {
            // atan rather than acos(dot): for a small or distant light the edge subtends a tiny
            // angle, where acos near 1 loses most of its precision in fp32.
            sum += atan(sinAngle, dot(a, b)) * dot(edgeNormal / sinAngle, N);
        }
    }
    return max(0.5 * sum, 0.0);
}

// The specular BRDF value of a rectangular light for a surface with normal N and this roughness,
// at the light's representative point, and the Fresnel term it used (the base's diffuse weight
// needs it). The caller multiplies by the irradiance.
vec3 AreaLightSpecular(
    vec3 worldPos, vec3 N, vec3 V, float roughness, vec3 F0,
    vec3 center, vec3 lightNormal, vec3 rightAxis, vec3 upAxis, vec2 halfSize, float area,
    out vec3 F)
{
    // Representative point: where the reflection ray meets the light's plane, clamped into the
    // rectangle. A ray that never reaches the plane falls back to the receiver's projection onto it.
    vec3 R = reflect(-V, N);
    float rayDotNormal = dot(R, lightNormal);
    vec3 planePoint = rayDotNormal < -1e-4
                          ? worldPos + R * (dot(center - worldPos, lightNormal) / rayDotNormal)
                          : worldPos - lightNormal * dot(worldPos - center, lightNormal);
    vec3 local = planePoint - center;
    vec3 closest = center +
                   rightAxis * clamp(dot(local, rightAxis), -halfSize.x, halfSize.x) +
                   upAxis * clamp(dot(local, upAxis), -halfSize.y, halfSize.y);

    vec3 toLight = closest - worldPos;
    float lightDistance = max(length(toLight), 0.0001);
    vec3 L = toLight / lightDistance;

    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    // Moving L to the representative point already spreads the highlight over the light's shape,
    // so the lobe itself is left alone and only renormalized: (alpha / widened)^2, with alpha
    // widened by the half angle of a disc with the rectangle's area. Without it the highlight's
    // peak would stay a point light's while its footprint grew, adding energy.
    float alpha = roughness * roughness;
    float equivalentRadius = sqrt(area / PI);
    float widenedAlpha = clamp(alpha + equivalentRadius / (2.0 * lightDistance), alpha, 1.0);
    float energyNormalization = (alpha * alpha) / (widenedAlpha * widenedAlpha);

    float D = DistributionGGX(N, H, roughness) * energyNormalization;
    float G = GeometrySmith(N, V, L, roughness);
    return NdL > 0.0 ? (D * G * F) / max(4.0 * NdV * NdL, 0.0001) : vec3(0.0);
}

// A one-sided Lambertian rectangle. Diffuse uses the exact irradiance from RectangleFormFactor.
// Specular uses a representative point, the point on the rectangle closest to the reflection
// ray, renormalized for the angle the light subtends so the enlarged highlight does not add
// energy. As the rectangle shrinks this converges to a point light emitting
// flux / pi along its normal with a cosine falloff. The coat, when there is one, gets the same
// treatment with its own normal and roughness, written to coatContribution.
vec3 EvaluateAreaLight(
    SceneLightData light,
    vec3 worldPos,
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 energyCompensation,
    CoatParams coat,
    out vec3 coatContribution)
{
    coatContribution = vec3(0.0);
    vec3 center = light.positionAndRange.xyz;
    vec3 lightNormal = normalize(light.directionAndType.xyz);
    vec3 rightAxis = normalize(light.areaRightAxis.xyz);
    vec3 upAxis = cross(lightNormal, rightAxis);
    vec2 halfSize = 0.5 * max(light.spotAndArea.zw, vec2(0.001));

    // One sided: the back of the rectangle emits nothing.
    if (dot(worldPos - center, lightNormal) <= 0.0)
        return vec3(0.0);

    vec3 corners[4];
    corners[0] = center - rightAxis * halfSize.x - upAxis * halfSize.y;
    corners[1] = center + rightAxis * halfSize.x - upAxis * halfSize.y;
    corners[2] = center + rightAxis * halfSize.x + upAxis * halfSize.y;
    corners[3] = center - rightAxis * halfSize.x + upAxis * halfSize.y;

    // Lumens to luminance for a Lambertian emitter: flux / (pi * area).
    float area = 4.0 * halfSize.x * halfSize.y;
    vec3 luminance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (PI * area));
    float window = RangeWindow(distance(worldPos, center), light.positionAndRange.w);

    if (coat.factor > 0.0)
    {
        float coatFormFactor = RectangleFormFactor(worldPos, coat.normal, corners);
        if (coatFormFactor > 0.0)
        {
            vec3 coatFresnel;
            coatContribution = AreaLightSpecular(
                                   worldPos, coat.normal, V, coat.roughness, COAT_F0,
                                   center, lightNormal, rightAxis, upAxis, halfSize, area,
                                   coatFresnel) *
                               (luminance * coatFormFactor * window);
        }
    }

    float formFactor = RectangleFormFactor(worldPos, N, corners);
    if (formFactor <= 0.0)
        return vec3(0.0);

    vec3 irradiance = luminance * formFactor * window;

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F;
    vec3 specular = AreaLightSpecular(
                        worldPos, N, V, roughness, F0,
                        center, lightNormal, rightAxis, upAxis, halfSize, area,
                        F) *
                    energyCompensation;

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * irradiance;
}

// ---------------------------------------------------------------------------
// Directional shadow
// ---------------------------------------------------------------------------

// How far, in texels of the cascade in use, the lookup moves off the surface along its geometric
// normal. Moving the receiver rather than biasing its depth keeps acne off surfaces at grazing
// angles to the light without detaching shadows from the casters' feet ("peter panning").
const float kShadowNormalOffsetTexels = 1.5;

// The last fraction of each cascade's depth range fades into the next cascade, or into no shadow
// after the last one, so neither the cascade switch nor the shadow distance shows as a hard line.
const float kShadowCascadeBlendFraction = 0.1;

// 3x3 taps, each a bilinear comparison: an even 4x4 texel filter.
float SampleShadowCascade(int cascade, vec3 worldPos, vec3 geoNormal)
{
    vec3 offsetPos = worldPos + geoNormal * (ubo.shadowCascadeTexelSizes[cascade] * kShadowNormalOffsetTexels);
    vec4 lightClip = ubo.shadowCascadeViewProjection[cascade] * vec4(offsetPos, 1.0);
    vec2 uv = lightClip.xy * 0.5 + 0.5;
    float texel = ubo.shadowParams.y;

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            // Explicit zero gradients: the lookup runs in non-uniform control flow, where implicit
            // derivatives are undefined, and the map has a single mip level anyway.
            lit += textureGrad(
                shadowMap,
                vec4(uv + vec2(x, y) * texel, float(cascade), lightClip.z),
                vec2(0.0),
                vec2(0.0));
        }
    }
    return lit / 9.0;
}

// The fraction of the shadow casting light that reaches this point: 1 lit, 0 in shadow.
float EvaluateDirectionalShadow(vec3 worldPos, vec3 geoNormal)
{
    float viewDepth = -(ubo.view * vec4(worldPos, 1.0)).z;
    int cascade = -1;
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i)
    {
        if (viewDepth <= ubo.shadowCascadeSplits[i])
        {
            cascade = i;
            break;
        }
    }
    if (cascade < 0)
        return 1.0;

    float lit = SampleShadowCascade(cascade, worldPos, geoNormal);

    float splitFar = ubo.shadowCascadeSplits[cascade];
    float splitNear = cascade == 0 ? 0.0 : ubo.shadowCascadeSplits[cascade - 1];
    float blendWidth = (splitFar - splitNear) * kShadowCascadeBlendFraction;
    float blend = clamp((viewDepth - (splitFar - blendWidth)) / blendWidth, 0.0, 1.0);
    if (blend > 0.0)
    {
        float nextLit = cascade + 1 < SHADOW_CASCADE_COUNT
                            ? SampleShadowCascade(cascade + 1, worldPos, geoNormal)
                            : 1.0;
        lit = mix(lit, nextLit, blend);
    }
    return lit;
}

// ---------------------------------------------------------------------------
// Uniform ambient
// ---------------------------------------------------------------------------

// Karis' analytic fit of the split-sum environment BRDF ("Physically Based Shading on Mobile",
// 2014). Returns the scale and bias that turn F0 into the directional albedo of the GGX lobe:
// specular albedo = F0 * x + y.
vec2 EnvironmentBrdfApprox(float roughness, float NdV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Scales a single-scattering GGX specular term up by the energy the lobe loses to light bouncing
// between microfacets more than once (Fdez-Aguera 2019, in Filament's form). environmentBrdf is
// the (A, B) pair the specular term was computed with: A + B is the lobe's directional albedo for
// F0 = 1, so a perfect conductor then reflects exactly all it receives. Rough metals gain the most;
// dielectrics, with their small F0, barely change. SpecularEnergyCompensation in
// engine/renderer/environment_brdf.cpp is the same formula and carries the tests.
vec3 SpecularEnergyCompensation(vec3 F0, vec2 environmentBrdf)
{
    float singleScatterAlbedo = max(environmentBrdf.x + environmentBrdf.y, 1e-4);
    return vec3(1.0) + F0 * (1.0 / singleScatterAlbedo - 1.0);
}

// Outgoing radiance from an environment of the same luminance in every direction. For that
// environment the split sum is exact apart from the BRDF fit: the prefiltered radiance is the
// luminance itself whatever the roughness. Metals get only the specular lobe, tinted by their F0,
// and the diffuse lobe keeps whatever energy the specular one did not reflect.
vec3 EvaluateUniformAmbient(
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 luminance)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = EnvironmentBrdfApprox(roughness, NdV);
    // Compensated with the same fit it was computed from, so the white furnace holds within it.
    vec3 specularAlbedo = (F0 * environmentBrdf.x + environmentBrdf.y) * SpecularEnergyCompensation(F0, environmentBrdf);
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    return (diffuseAlbedo + specularAlbedo) * luminance;
}

// ---------------------------------------------------------------------------
// Per-light contribution
// ---------------------------------------------------------------------------
vec3 EvaluateSceneLight(
    SceneLightData light,
    vec3 worldPos,
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 energyCompensation,
    CoatParams coat,
    out vec3 coatContribution)
{
    coatContribution = vec3(0.0);
    int lightType = int(light.directionAndType.w);

    if (lightType == LIGHT_AMBIENT)
        return vec3(0.0); // handled as ambient term below

    vec3 L;
    vec3 radiance;

    if (lightType == LIGHT_DIRECTIONAL)
    {
        // Direction stored is the world direction the light travels; negate for L.
        L = normalize(-light.directionAndType.xyz);
        // Intensity is in lux (irradiance on a surface).
        radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
    }
    else if (lightType == LIGHT_POINT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);
        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);
        // Lumens → lux at surface: I / (4π), then attenuate.
        radiance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (4.0 * PI)) * att;
    }
    else if (lightType == LIGHT_SPOT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);

        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);

        vec3 spotDir = normalize(light.directionAndType.xyz);
        float cosAngle = dot(-L, spotDir);
        float innerCos = light.spotAndArea.x;
        float outerCos = light.spotAndArea.y;
        float spotAtt = clamp((cosAngle - outerCos) / max(innerCos - outerCos, 0.0001), 0.0, 1.0);
        spotAtt *= spotAtt;

        // Lumens to candela over the solid angle of the outer cone. Light keeps reaching out to the
        // outer angle, so dividing by the inner cone alone would emit more flux than authored, and
        // more the softer the edge.
        float coneOmega = max(2.0 * PI * (1.0 - outerCos), 0.0001);
        radiance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / coneOmega) * att * spotAtt;
    }
    else if (lightType == LIGHT_AREA)
    {
        // Integrates over the rectangle itself, so it does not go through EvaluateBRDF.
        return EvaluateAreaLight(light, worldPos, N, V, albedo, metallic, roughness, energyCompensation, coat, coatContribution);
    }
    else
    {
        return vec3(0.0);
    }

    if (coat.factor > 0.0)
    {
        coatContribution = EvaluateCoatSpecular(coat, V, L, radiance);
    }
    return EvaluateBRDF(N, V, L, albedo, metallic, roughness, energyCompensation, radiance);
}

// Sky irradiance for a direction from the active sky's SH: the atmosphere's (computed on the GPU)
// or the HDRI's (projected on the CPU, in the camera block).
vec3 EvaluateSkyIrradiance(vec3 direction)
{
    float basis[9];
    EvaluateShBasis(direction, basis);
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    vec3 irradiance = vec3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        vec3 coefficient = hdri ? ubo.hdriIrradianceSh[i].rgb : skyIrradiance.coefficients[i].rgb;
        irradiance += coefficient * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, vec3(0.0));
}

// The DFG table at (roughness, N.V), clamped to texel centres so the lookup never wraps.
vec2 SampleEnvironmentBrdf(float roughness, float NdV)
{
    const float size = 64.0;
    vec2 uv = clamp(vec2(NdV, roughness), vec2(0.5 / size), vec2(1.0 - 0.5 / size));
    return textureLod(environmentBrdfLut, uv, 0.0).rg;
}

// The ambient term under a physical sky, split-sum (Karis 2013): the diffuse lobe sees the SH
// irradiance for N, the specular lobe the GGX-prefiltered sky along R at the surface's roughness,
// weighted by the DFG table's F0 A + B. The scene's Ambient lights, but not the fallback, add their
// uniform luminance.
vec3 EvaluateSkyAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    vec3 specularAlbedo = (F0 * environmentBrdf.x + environmentBrdf.y) * SpecularEnergyCompensation(F0, environmentBrdf);
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    vec3 R = reflect(-V, N);
    vec3 specular = textureLod(prefilteredEnvironment, R, roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    vec3 sky = diffuseAlbedo * EvaluateSkyIrradiance(N) / ATMOSPHERE_PI + specularAlbedo * specular;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return sky + (diffuseAlbedo + specularAlbedo) * sceneAmbient;
}

// ---------------------------------------------------------------------------
// Light clusters
// ---------------------------------------------------------------------------

// The cluster a world position falls in: its tile from the NDC the camera projects it to, its slice
// from its view depth. FindLightCluster in engine/renderer/light_clusters.cpp is the same arithmetic,
// and the CPU binned the lights through the same projection, Y flip included, so the two agree on
// which tile a pixel is in.
uint FindLightCluster(vec3 worldPosition)
{
    vec4 viewPosition = ubo.view * vec4(worldPosition, 1.0);
    vec4 clip = ubo.proj * viewPosition;
    vec2 ndc = clip.xy / clip.w;
    vec2 tileCount = vec2(LIGHT_CLUSTER_TILES_X, LIGHT_CLUSTER_TILES_Y);
    uvec2 tile = uvec2(clamp(floor((ndc * 0.5 + 0.5) * tileCount), vec2(0.0), tileCount - 1.0));
    float slice = floor(log(max(-viewPosition.z, 1e-4)) * ubo.lightClusterSlices.x + ubo.lightClusterSlices.y);
    uint sliceIndex = uint(clamp(slice, 0.0, float(LIGHT_CLUSTER_SLICES - 1)));
    return (sliceIndex * LIGHT_CLUSTER_TILES_Y + tile.y) * LIGHT_CLUSTER_TILES_X + tile.x;
}

// The coat's share of the ambient term: its lobe's directional albedo (0.04 A + B) times what the
// environment sends along the coat's reflection, the same split sum the base's specular uses. Under
// the uniform ambient that is the ambient luminance itself, weighted by Karis' fit as the base is.
vec3 EvaluateCoatAmbient(CoatParams coat, vec3 V)
{
    float NdV = max(dot(coat.normal, V), 0.0);
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        vec2 environmentBrdf = EnvironmentBrdfApprox(coat.roughness, NdV);
        return (COAT_F0 * environmentBrdf.x + environmentBrdf.y) * ubo.ambientLuminance.rgb;
    }
    vec2 environmentBrdf = SampleEnvironmentBrdf(coat.roughness, NdV);
    vec3 coatAlbedo = COAT_F0 * environmentBrdf.x + environmentBrdf.y;
    vec3 R = reflect(-V, coat.normal);
    vec3 sky = textureLod(prefilteredEnvironment, R, coat.roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return coatAlbedo * (sky + sceneAmbient);
}

// Ambient plus every direct light for one resolved surface point; the caller adds emissive. The
// arithmetic and its order are exactly what triangle.frag's main() ran inline before phase two,
// (ambient + direct) with emissive added afterwards, so the forward image is unchanged by the move
// and the deferred path shares it by construction rather than by copy.
//
// N is the shading normal, normal map applied. geoNormal is the interpolated vertex normal,
// flipped for back faces the way triangle.frag flips it: the shadow lookup offsets along it, not
// along N, so the shadow boundary does not follow the normal map.
vec3 ShadeSurface(
    vec3 worldPosition, vec3 N, vec3 geoNormal, vec3 V,
    vec3 albedo, float metallic, float roughness, float ao,
    vec3 emissive,
    CoatParams coat)
{
    // The CPU has already summed the scene's Ambient lights into ambientLuminance, or put the
    // fallback there when there are none; see SelectSceneLights. Under a physical sky the sky's
    // irradiance takes the fallback's place.
    vec3 ambient = EnvironmentMode() == ENVIRONMENT_NONE
                       ? EvaluateUniformAmbient(N, V, albedo, metallic, roughness, ubo.ambientLuminance.rgb)
                       : EvaluateSkyAmbient(N, V, albedo, metallic, roughness);
    ambient *= ao;

    // One factor for every direct light: it depends only on the surface and the view. It comes
    // from the DFG table, whose visibility term remaps k = alpha / 2 where the direct lights'
    // GeometrySchlickGGX uses (roughness + 1)^2 / 8, so it is the table lobe's loss standing in
    // for theirs; the two differ little and the table is what the specular IBL uses.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 energyCompensation = SpecularEnergyCompensation(F0, SampleEnvironmentBrdf(roughness, max(dot(N, V), 0.0)));

    // Directional lights reach everywhere, so they are never binned: every pixel loops over them.
    // The shadow caster is always one of them.
    uint directionalCount = ubo.lightCounts.x;
    int shadowLightIndex = int(ubo.shadowParams.x);
    vec3 directAccum = vec3(0.0);
    vec3 coatAccum = vec3(0.0);
    vec3 coatContribution;
    for (uint i = 0u; i < directionalCount; ++i)
    {
        vec3 contribution = EvaluateSceneLight(
            sceneLights.lights[i],
            worldPosition,
            N, V,
            albedo, metallic, roughness,
            energyCompensation,
            coat,
            coatContribution);
        // Skipped where the light contributes nothing, which includes every surface facing away
        // from it: those are dark already, and the lookup is the most expensive part of the loop.
        // The coat's normal can face the light where the base's does not, so it counts too.
        if (int(i) == shadowLightIndex &&
            (any(greaterThan(contribution, vec3(0.0))) || any(greaterThan(coatContribution, vec3(0.0)))))
        {
            float shadow = EvaluateDirectionalShadow(worldPosition, geoNormal);
            contribution *= shadow;
            coatContribution *= shadow;
        }
        directAccum += contribution;
        coatAccum += coatContribution;
    }

    // Local lights: the pixel's cluster lists every one whose range reaches it, in ascending order,
    // and a light left out contributes exactly zero, so this sums the same values in the same order
    // as the brute-force loop below. That loop stays as the comparison path.
    if (ubo.lightCounts.z != 0u)
    {
        uvec2 range = lightClusters.ranges[FindLightCluster(worldPosition)];
        for (uint k = 0u; k < range.y; ++k)
        {
            directAccum += EvaluateSceneLight(
                sceneLights.lights[lightClusters.indices[range.x + k]],
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution);
            coatAccum += coatContribution;
        }
    }
    else
    {
        for (uint i = directionalCount; i < ubo.lightCounts.y; ++i)
        {
            directAccum += EvaluateSceneLight(
                sceneLights.lights[i],
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution);
            coatAccum += coatContribution;
        }
    }

    // The base, emissive included, loses what the coat's Fresnel reflects toward the viewer; the
    // coat adds its own lobe. Uncoated surfaces sum exactly what they did before emissive moved in
    // here: (ambient + direct) + emissive.
    vec3 color = ambient + directAccum + emissive;
    if (coat.factor > 0.0)
    {
        float coatFresnel = FresnelSchlick(max(dot(coat.normal, V), 0.0), COAT_F0).x;
        vec3 coatAmbient = EvaluateCoatAmbient(coat, V) * ao;
        color = color * (1.0 - coat.factor * coatFresnel) + coat.factor * (coatAmbient + coatAccum);
    }
    return color;
}

#endif
