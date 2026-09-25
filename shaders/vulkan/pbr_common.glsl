#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

// SceneLightData, the LIGHT_* and SHADOW_CASCADE_COUNT constants and the ubo block.
#include "scene_common.glsl"
#include "ssr_common.glsl"
#include "local_shadow_common.glsl"
#include "anisotropy_common.glsl"
#include "brdf_common.glsl"
#include "ltc_common.glsl"
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

// The local lights' shadow atlas (see VulkanLocalShadowPass), sampled with a LESS_OR_EQUAL depth
// comparison, and its tiles (GpuLocalShadowTile). A light's areaRightAxis.w is 1 + its first tile,
// 0 when it casts no shadow; a point or area light's six cube faces follow in SelectCubeFace order.
layout(set = 0, binding = 13) uniform sampler2DShadow localShadowAtlas;

struct LocalShadowTileData
{
    mat4 viewProjection;
    vec4 atlasRect; // uv offset xy, uv size zw
    vec4 params;    // x = world size of one texel at one metre from the light, y = 1 on a cube's tiles
};

layout(set = 0, binding = 14, std430) readonly buffer LocalShadowTileBuffer
{
    LocalShadowTileData tiles[];
}
localShadowTiles;

// The area lights' LTC tables (engine/renderer/ltc_table.h): the inverse matrices and the lobe's
// (albedo, Fresnel share), 64 x 64 RGBA32F, x = sqrt(1 - N.V), y = roughness.
layout(set = 0, binding = 15) uniform sampler2D ltcInverseMatrices;
layout(set = 0, binding = 16) uniform sampler2D ltcAmplitudes;

// Where (roughness, N.V) falls in the LTC tables, clamped to texel centres.
vec2 LtcTableUv(float roughness, float NdV)
{
    const float size = 64.0;
    return clamp(vec2(sqrt(1.0 - NdV), roughness), vec2(0.5 / size), vec2(1.0 - 0.5 / size));
}

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR microfacet BRDF helpers
// ---------------------------------------------------------------------------

// The denominator is at least alpha^2 (at the peak), and the 0.04 roughness floor keeps that above
// 2.6e-6, so it needs no clamp beyond keeping it positive. An earlier max(pi d^2, 1e-4) floor cut
// the peak of every lobe smoother than roughness 0.3 or so, by up to five orders of magnitude at
// the floor: smooth surfaces showed a dim blur where the sun should have been.
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = max(NdH2 * (a2 - 1.0) + 1.0, 1e-7);
    return a2 / (PI * denom * denom);
}

// D * G / (4 N.L N.V) of the isotropic GGX lobe: the height-correlated Smith visibility
// (brdf_common.glsl), the term the DFG table integrates.
float IsotropicSpecularTerm(vec3 N, vec3 V, vec3 L, vec3 H, float roughness)
{
    return DistributionGGX(N, H, roughness) *
           VisibilitySmithGgxCorrelated(max(dot(N, V), 1e-4), max(dot(N, L), 0.0), roughness * roughness);
}

// ---------------------------------------------------------------------------
// Light sources with a size
// ---------------------------------------------------------------------------

// What a direct light looks like to a specular lobe. L is the direction to the light's centre,
// which diffuse and sheen use. A directional light is a disk of angular radius asin(size) (the
// sun's, from the environment); a point or spot light a sphere of radius size metres around the
// end of toLight. Size 0 is a point, exactly as before sources had a size.
struct LightSource
{
    vec3 L;
    vec3 toLight;
    float size;
    bool directional;
};

// The direction a lobe around N sees the light along, the point of the source closest to its
// reflection vector, and in normalization the factor its peak falls by as the source spreads it.
vec3 SpecularLightDirection(LightSource source, vec3 N, vec3 V, float roughness, out float normalization)
{
    normalization = 1.0;
    if (source.size <= 0.0)
        return source.L;
    vec3 R = reflect(-V, N);
    float alpha = roughness * roughness;
    if (source.directional)
    {
        normalization = SourceSizeNormalization(alpha, source.size);
        return DiskLightSpecularDirection(source.L, R, sqrt(max(1.0 - source.size * source.size, 0.0)), source.size);
    }
    float distanceToCentre = max(length(source.toLight), 1e-4);
    normalization = SourceSizeNormalization(alpha, source.size / distanceToCentre);
    return normalize(SphereLightSpecularVector(source.toLight, R, source.size));
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------------------------------------------------------------------------
// Anisotropy (KHR_materials_anisotropy)
// ---------------------------------------------------------------------------

// The base's anisotropic stretch: tangent is the world direction the lobe widens along, in the
// surface of the shading normal; strength 0 is the isotropic base, and every anisotropic term below
// is skipped for it, so isotropic surfaces shade exactly as before.
struct AnisotropyParams
{
    vec3 tangent;
    float strength;
};

AnisotropyParams NoAnisotropy()
{
    AnisotropyParams anisotropy;
    anisotropy.tangent = vec3(1.0, 0.0, 0.0);
    anisotropy.strength = 0.0;
    return anisotropy;
}

// D * G / (4 N.L N.V) of the base's anisotropic lobe (the Khronos sample viewer's):
// alpha_t = mix(alpha, 1, strength^2) along the tangent, alpha across it.
float AnisotropicSpecularTerm(vec3 N, vec3 V, vec3 L, vec3 H, float roughness, AnisotropyParams anisotropy)
{
    vec3 T = anisotropy.tangent;
    vec3 B = cross(N, T);
    float alpha = roughness * roughness;
    float alphaT = mix(alpha, 1.0, anisotropy.strength * anisotropy.strength);
    float D = DistributionGgxAnisotropic(max(dot(N, H), 0.0), dot(T, H), dot(B, H), alphaT, alpha);
    float Vis = VisibilityGgxAnisotropic(
        max(dot(N, L), 0.0), max(dot(N, V), 1e-4), dot(T, V), dot(B, V), dot(T, L), dot(B, L), alphaT, alpha);
    return D * Vis;
}

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF contribution for one light sample
// Returns outgoing radiance.
// ---------------------------------------------------------------------------
// Diffuse toward the light's centre L, specular toward specularL (SpecularLightDirection), whose
// lobe is scaled by specularNormalization.
vec3 EvaluateBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 specularL, float specularNormalization,
    vec3 albedo, float metallic, float roughness,
    AnisotropyParams anisotropy,
    vec3 energyCompensation,
    vec3 radiance)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 result = vec3(0.0);

    float NdL = max(dot(N, L), 0.0);
    if (NdL > 0.0)
    {
        // Burley's diffuse (brdf_common.glsl), weighted by what the specular Fresnel toward the
        // centre leaves, as the Lambert term it replaced was.
        vec3 H = normalize(V + L);
        vec3 kD = (vec3(1.0) - FresnelSchlick(max(dot(H, V), 0.0), F0)) * (1.0 - metallic);
        result += kD * albedo * BurleyDiffuse(max(NdV, 1e-4), NdL, max(dot(L, H), 0.0), roughness) * radiance * NdL;
    }

    float specularNdL = max(dot(N, specularL), 0.0);
    if (specularNdL > 0.0)
    {
        vec3 H = normalize(V + specularL);
        vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        float term = anisotropy.strength > 0.0 ? AnisotropicSpecularTerm(N, V, specularL, H, roughness, anisotropy)
                                               : IsotropicSpecularTerm(N, V, specularL, H, roughness);
        result += term * specularNormalization * F * energyCompensation * radiance * specularNdL;
    }
    return result;
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

// The coat lobe's outgoing radiance for the light seen along specularL (SpecularLightDirection for
// the coat's normal and roughness), scaled by that lobe's normalization. Radiance is the light's
// illuminance at normal incidence, as EvaluateBRDF takes it.
vec3 EvaluateCoatSpecular(CoatParams coat, vec3 V, vec3 specularL, float specularNormalization, vec3 radiance)
{
    float NdL = max(dot(coat.normal, specularL), 0.0);
    if (NdL <= 0.0)
        return vec3(0.0);

    vec3 H = normalize(V + specularL);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), COAT_F0);
    return IsotropicSpecularTerm(coat.normal, V, specularL, H, coat.roughness) * specularNormalization * F * radiance * NdL;
}

// ---------------------------------------------------------------------------
// Sheen (KHR_materials_sheen), Filament's model
// ---------------------------------------------------------------------------

// Fibres standing off a cloth surface: the Charlie distribution (Estevez & Kulla 2017) with
// Neubelt's visibility, tinted by the sheen colour, on the shading normal. A black colour is no
// sheen, and every sheen term below is skipped for it.
struct SheenParams
{
    vec3 color;
    // Perceptual roughness, already floored at 0.04.
    float roughness;
};

SheenParams NoSheen()
{
    SheenParams sheen;
    sheen.color = vec3(0.0);
    sheen.roughness = 1.0;
    return sheen;
}

bool HasSheen(SheenParams sheen)
{
    return max(sheen.color.r, max(sheen.color.g, sheen.color.b)) > 0.0;
}

// sin^2 is floored as Filament floors it, so the 1 / alpha power stays finite at grazing half
// vectors. IntegrateSheenAlbedo in engine/renderer/environment_brdf.cpp integrates the same terms.
float DistributionCharlie(float roughness, float NdH)
{
    float alpha = roughness * roughness;
    float sin2h = max(1.0 - NdH * NdH, 0.0078125);
    return (2.0 + 1.0 / alpha) * pow(sin2h, 0.5 / alpha) / (2.0 * PI);
}

float VisibilityNeubelt(float NdV, float NdL)
{
    return 1.0 / (4.0 * max(NdL + NdV - NdL * NdV, 1e-4));
}

vec3 EvaluateSheen(vec3 N, vec3 V, vec3 L, SheenParams sheen, vec3 radiance)
{
    float NdL = max(dot(N, L), 0.0);
    if (NdL <= 0.0)
        return vec3(0.0);
    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    return sheen.color * (DistributionCharlie(sheen.roughness, max(dot(N, H), 0.0)) * VisibilityNeubelt(NdV, NdL)) * radiance * NdL;
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


// The specular BRDF value of a rectangular light for a surface with normal N and this roughness,
// at the light's representative point, and the Fresnel term it used (the base's diffuse weight
// needs it). The caller multiplies by the irradiance.
vec3 AreaLightSpecular(
    vec3 worldPos, vec3 N, vec3 V, float roughness, AnisotropyParams anisotropy, vec3 F0,
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

    if (NdL <= 0.0)
        return vec3(0.0);
    // An anisotropic base stretches the lobe at the same representative point, with the same
    // renormalisation.
    float term = anisotropy.strength > 0.0 ? AnisotropicSpecularTerm(N, V, L, H, roughness, anisotropy)
                                           : IsotropicSpecularTerm(N, V, L, H, roughness);
    return term * energyNormalization * F;
}

// A one-sided Lambertian rectangle, through linearly transformed cosines (Heitz et al. 2016,
// ltc_common.glsl, tables fitted to this lobe by tools/ltc_fit): diffuse integrates the clamped
// cosine over the rectangle clipped to the horizon, which is its exact irradiance, and the base's
// and the coat's specular integrate the fitted lobe over it. An anisotropic base, which the
// isotropic fit cannot follow, keeps the representative point of AreaLightSpecular. Sheen, broad
// enough not to care, is evaluated toward the centre.
vec3 EvaluateAreaLight(
    SceneLightData light,
    vec3 worldPos,
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 energyCompensation,
    CoatParams coat,
    out vec3 coatContribution,
    SheenParams sheen,
    out vec3 sheenContribution,
    AnisotropyParams anisotropy)
{
    coatContribution = vec3(0.0);
    sheenContribution = vec3(0.0);
    vec3 center = light.positionAndRange.xyz;
    vec3 lightNormal = normalize(light.directionAndType.xyz);
    vec3 rightAxis = normalize(light.areaRightAxis.xyz);
    vec3 upAxis = cross(lightNormal, rightAxis);
    vec2 halfSize = 0.5 * max(light.spotAndArea.zw, vec2(0.001));

    // One sided: the back of the rectangle emits nothing.
    if (dot(worldPos - center, lightNormal) <= 0.0)
        return vec3(0.0);

    // Relative to the shaded point, which the LTC integration treats as the origin.
    vec3 c0 = center - rightAxis * halfSize.x - upAxis * halfSize.y - worldPos;
    vec3 c1 = center + rightAxis * halfSize.x - upAxis * halfSize.y - worldPos;
    vec3 c2 = center + rightAxis * halfSize.x + upAxis * halfSize.y - worldPos;
    vec3 c3 = center - rightAxis * halfSize.x + upAxis * halfSize.y - worldPos;

    // Lumens to luminance for a Lambertian emitter: flux / (pi * area).
    float area = 4.0 * halfSize.x * halfSize.y;
    vec3 luminance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (PI * area)) *
                     RangeWindow(distance(worldPos, center), light.positionAndRange.w);

    if (coat.factor > 0.0)
    {
        vec2 uv = LtcTableUv(coat.roughness, max(dot(coat.normal, V), 0.0));
        mat3 minv = LtcInverseMatrix(textureLod(ltcInverseMatrices, uv, 0.0), coat.normal, V);
        vec2 amplitude = textureLod(ltcAmplitudes, uv, 0.0).rg;
        float coverage = LtcIntegrateQuad(minv * c0, minv * c1, minv * c2, minv * c3);
        coatContribution = luminance * (coverage * (COAT_F0 * amplitude.x + (1.0 - COAT_F0) * amplitude.y));
    }

    // The clipped cosine's integral is the form factor over pi; times pi and the luminance it is the
    // irradiance.
    mat3 cosineFrame = LtcInverseMatrix(vec4(1.0, 0.0, 0.0, 1.0), N, V);
    float formFactor = LtcIntegrateQuad(cosineFrame * c0, cosineFrame * c1, cosineFrame * c2, cosineFrame * c3);
    if (formFactor <= 0.0)
        return vec3(0.0);
    vec3 irradiance = luminance * (PI * formFactor);

    vec3 toCentre = normalize(center - worldPos);
    vec3 centreH = normalize(V + toCentre);
    float NdV = max(dot(N, V), 1e-4);
    if (HasSheen(sheen))
    {
        sheenContribution = sheen.color *
                            (DistributionCharlie(sheen.roughness, max(dot(N, centreH), 0.0)) *
                             VisibilityNeubelt(NdV, max(dot(N, toCentre), 1e-4))) *
                            irradiance;
    }

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 specular;
    if (anisotropy.strength > 0.0)
    {
        vec3 F;
        specular = AreaLightSpecular(
                       worldPos, N, V, roughness, anisotropy, F0,
                       center, lightNormal, rightAxis, upAxis, halfSize, area,
                       F) *
                   irradiance;
    }
    else
    {
        vec2 uv = LtcTableUv(roughness, NdV);
        mat3 minv = LtcInverseMatrix(textureLod(ltcInverseMatrices, uv, 0.0), N, V);
        vec2 amplitude = textureLod(ltcAmplitudes, uv, 0.0).rg;
        float coverage = LtcIntegrateQuad(minv * c0, minv * c1, minv * c2, minv * c3);
        specular = luminance * coverage * (F0 * amplitude.x + (vec3(1.0) - F0) * amplitude.y);
    }
    specular *= energyCompensation;

    // Burley's diffuse, weighted by the Fresnel toward the centre, applied to the exact irradiance.
    vec3 kD = (vec3(1.0) - FresnelSchlick(max(dot(V, centreH), 0.0), F0)) * (1.0 - metallic);
    vec3 diffuse = kD * albedo * BurleyDiffuse(NdV, clamp(dot(N, toCentre), 1e-4, 1.0), max(dot(toCentre, centreH), 0.0), roughness) * irradiance;

    return diffuse + specular;
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
// Local light shadows
// ---------------------------------------------------------------------------

// The fraction of a local light that reaches this point: 1 lit, 0 in shadow. The light must have a
// tile (areaRightAxis.w > 0).
float EvaluateLocalShadow(SceneLightData light, vec3 worldPos, vec3 geoNormal)
{
    vec3 fromLight = worldPos - light.positionAndRange.xyz;
    int tileIndex = int(light.areaRightAxis.w) - 1;
    // The planner marks the first tile of a cube; a spot light may have been given one too.
    if (localShadowTiles.tiles[tileIndex].params.y > 0.5)
        tileIndex += SelectCubeFace(fromLight);
    LocalShadowTileData tile = localShadowTiles.tiles[tileIndex];

    // The cascades' normal offset, with a texel that grows with the distance to the light.
    float texel = tile.params.x * max(length(fromLight), kLocalShadowNearPlaneMetres);
    vec3 offsetPos = worldPos + geoNormal * (texel * kShadowNormalOffsetTexels);
    vec3 coords = LocalShadowAtlasCoordinates(tile.viewProjection * vec4(offsetPos, 1.0), tile.atlasRect, kLocalShadowGuardFraction);

    float atlasTexel = tile.atlasRect.z / kLocalShadowTileTexels;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            // Zero gradients for the same reason as the cascades: non-uniform control flow.
            lit += textureGrad(localShadowAtlas, vec3(coords.xy + vec2(x, y) * atlasTexel, coords.z), vec2(0.0), vec2(0.0));
        }
    }
    return lit / 9.0;
}

// ---------------------------------------------------------------------------
// Uniform ambient
// ---------------------------------------------------------------------------

// The DFG table at (roughness, N.V), clamped to texel centres so the lookup never wraps.
vec2 SampleEnvironmentBrdf(float roughness, float NdV)
{
    const float size = 64.0;
    vec2 uv = clamp(vec2(NdV, roughness), vec2(0.5 / size), vec2(1.0 - 0.5 / size));
    return textureLod(environmentBrdfLut, uv, 0.0).rg;
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
// The specular lobe's incoming radiance: the environment's, occluded by the AO as a lobe of this
// roughness is (Lagarde 2014), replaced by the screen-space reflection as far as it is trusted.
// reflection.rgb is physical radiance, reflection.a its confidence.
// horizon is HorizonSpecularOcclusion for the reflection vector: the environment below the
// geometric surface is blocked by it. Screen-space reflections see the real geometry and are not
// faded.
vec3 SpecularAmbientRadiance(vec3 environment, vec4 reflection, float NdV, float ao, float roughness, float horizon)
{
    return mix(environment * (SpecularOcclusion(NdV, ao, roughness) * horizon), reflection.rgb, reflection.a);
}

vec3 EvaluateUniformAmbient(
    vec3 N, vec3 geoNormal, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 luminance,
    float ao,
    vec4 reflection)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    // The DFG table, as under a physical sky: Karis' analytic fit, used here before, approximated the
    // Smith-Schlick table and no longer matches the correlated lobe the direct lights draw.
    vec2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    // Compensated with the same table it was computed from, so the white furnace holds.
    vec3 specularAlbedo = (F0 * environmentBrdf.x + environmentBrdf.y) * SpecularEnergyCompensation(F0, environmentBrdf);
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    float horizon = HorizonSpecularOcclusion(reflect(-V, N), geoNormal);
    return diffuseAlbedo * luminance * ao + specularAlbedo * SpecularAmbientRadiance(luminance, reflection, NdV, ao, roughness, horizon);
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
    out vec3 coatContribution,
    SheenParams sheen,
    out vec3 sheenContribution,
    AnisotropyParams anisotropy)
{
    coatContribution = vec3(0.0);
    sheenContribution = vec3(0.0);
    int lightType = int(light.directionAndType.w);

    if (lightType == LIGHT_AMBIENT)
        return vec3(0.0); // handled as ambient term below

    vec3 L;
    vec3 radiance;
    LightSource source;
    source.toLight = vec3(0.0);
    source.size = 0.0;
    source.directional = false;

    if (lightType == LIGHT_DIRECTIONAL)
    {
        // Direction stored is the world direction the light travels; negate for L.
        L = normalize(-light.directionAndType.xyz);
        // Every directional light is a disk the size of the environment's sun.
        source.directional = true;
        source.size = sqrt(max(1.0 - ubo.sunIlluminance.w * ubo.sunIlluminance.w, 0.0));
        // Intensity is in lux (irradiance on a surface).
        radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
    }
    else if (lightType == LIGHT_POINT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        source.toLight = toLight;
        source.size = light.spotAndArea.z;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);
        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);
        // Lumens → lux at surface: I / (4π), then attenuate.
        radiance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (4.0 * PI)) * att;
    }
    else if (lightType == LIGHT_SPOT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        source.toLight = toLight;
        source.size = light.spotAndArea.z;
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
        return EvaluateAreaLight(
            light, worldPos, N, V, albedo, metallic, roughness, energyCompensation,
            coat, coatContribution, sheen, sheenContribution, anisotropy);
    }
    else
    {
        return vec3(0.0);
    }

    source.L = L;
    if (coat.factor > 0.0)
    {
        float coatNormalization;
        vec3 coatL = SpecularLightDirection(source, coat.normal, V, coat.roughness, coatNormalization);
        coatContribution = EvaluateCoatSpecular(coat, V, coatL, coatNormalization, radiance);
    }
    if (HasSheen(sheen))
    {
        sheenContribution = EvaluateSheen(N, V, L, sheen, radiance);
    }
    float specularNormalization;
    vec3 specularL = SpecularLightDirection(source, N, V, roughness, specularNormalization);
    return EvaluateBRDF(N, V, L, specularL, specularNormalization, albedo, metallic, roughness, anisotropy, energyCompensation, radiance);
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


// The ambient term under a physical sky, split-sum (Karis 2013): the diffuse lobe sees the SH
// irradiance for N, the specular lobe the GGX-prefiltered sky along R at the surface's roughness,
// weighted by the DFG table's F0 A + B. The scene's Ambient lights, but not the fallback, add their
// uniform luminance.
vec3 EvaluateSkyAmbient(vec3 N, vec3 geoNormal, vec3 V, vec3 albedo, float metallic, float roughness, AnisotropyParams anisotropy, float ao, vec4 reflection)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    vec3 specularAlbedo = (F0 * environmentBrdf.x + environmentBrdf.y) * SpecularEnergyCompensation(F0, environmentBrdf);
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    // An anisotropic base reflects about the bent normal (AnisotropicBentNormal); the DFG weight
    // keeps the isotropic roughness.
    vec3 R = reflect(-V, anisotropy.strength > 0.0 ? AnisotropicBentNormal(N, V, anisotropy.tangent, anisotropy.strength, roughness) : N);
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    vec3 environment = textureLod(prefilteredEnvironment, R, roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb + sceneAmbient;
    vec3 diffuse = diffuseAlbedo * (EvaluateSkyIrradiance(N) / ATMOSPHERE_PI + sceneAmbient) * ao;
    return diffuse + specularAlbedo * SpecularAmbientRadiance(environment, reflection, NdV, ao, roughness, HorizonSpecularOcclusion(R, geoNormal));
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

// The sheen lobe's directional albedo, IntegrateSheenAlbedo clamped to 1, from the DFG table's
// blue channel.
float SampleSheenAlbedo(float roughness, float NdV)
{
    const float size = 64.0;
    vec2 uv = clamp(vec2(NdV, roughness), vec2(0.5 / size), vec2(1.0 - 0.5 / size));
    return textureLod(environmentBrdfLut, uv, 0.0).b;
}

// The sheen's share of the ambient term, as Filament shades it: the GGX-prefiltered sky along the
// reflection at the sheen's roughness (the uniform ambient under None), times the sheen colour and
// its albedo. The caller applies the occlusion.
vec3 EvaluateSheenAmbient(vec3 N, vec3 V, SheenParams sheen)
{
    float albedo = SampleSheenAlbedo(sheen.roughness, max(dot(N, V), 0.0));
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        return sheen.color * albedo * ubo.ambientLuminance.rgb;
    }
    vec3 R = reflect(-V, N);
    vec3 sky = textureLod(prefilteredEnvironment, R, sheen.roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return sheen.color * albedo * (sky + sceneAmbient);
}

// The coat's share of the ambient term: its lobe's directional albedo (0.04 A + B) times what the
// environment sends along the coat's reflection, the same split sum the base's specular uses. Under
// the uniform ambient that is the ambient luminance itself, weighted by the table as the base is.
vec3 EvaluateCoatAmbient(CoatParams coat, vec3 V)
{
    float NdV = max(dot(coat.normal, V), 0.0);
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        vec2 environmentBrdf = SampleEnvironmentBrdf(coat.roughness, NdV);
        return (COAT_F0 * environmentBrdf.x + environmentBrdf.y) * ubo.ambientLuminance.rgb;
    }
    vec2 environmentBrdf = SampleEnvironmentBrdf(coat.roughness, NdV);
    vec3 coatAlbedo = COAT_F0 * environmentBrdf.x + environmentBrdf.y;
    vec3 R = reflect(-V, coat.normal);
    vec3 sky = textureLod(prefilteredEnvironment, R, coat.roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return coatAlbedo * (sky + sceneAmbient);
}

// Darkens a local light's contributions by its shadow, where it has a tile and lights anything:
// like the directional caster, the lookup is skipped where the light contributes nothing.
void ApplyLocalShadow(
    SceneLightData light, vec3 worldPosition, vec3 geoNormal,
    inout vec3 contribution, inout vec3 coatContribution, inout vec3 sheenContribution)
{
    if (light.areaRightAxis.w > 0.5 &&
        (any(greaterThan(contribution, vec3(0.0))) || any(greaterThan(coatContribution, vec3(0.0)))))
    {
        float shadow = EvaluateLocalShadow(light, worldPosition, geoNormal);
        contribution *= shadow;
        coatContribution *= shadow;
        sheenContribution *= shadow;
    }
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
    CoatParams coat,
    SheenParams sheen,
    AnisotropyParams anisotropy,
    vec4 reflection)
{
    // The CPU has already summed the scene's Ambient lights into ambientLuminance, or put the
    // fallback there when there are none; see SelectSceneLights. Under a physical sky the sky's
    // irradiance takes the fallback's place.
    // The AO darkens the diffuse lobe directly and the specular one through SpecularOcclusion; a
    // screen-space reflection (reflection.a > 0) replaces the occluded environment where it is trusted.
    vec3 ambient = EnvironmentMode() == ENVIRONMENT_NONE
                       ? EvaluateUniformAmbient(N, geoNormal, V, albedo, metallic, roughness, ubo.ambientLuminance.rgb, ao, reflection)
                       : EvaluateSkyAmbient(N, geoNormal, V, albedo, metallic, roughness, anisotropy, ao, reflection);

    // One factor for every direct light: it depends only on the surface and the view. It comes
    // from the DFG table, which integrates the same height-correlated lobe the direct lights draw,
    // so it adds back exactly the energy that lobe loses.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 energyCompensation = SpecularEnergyCompensation(F0, SampleEnvironmentBrdf(roughness, max(dot(N, V), 0.0)));

    // Directional lights reach everywhere, so they are never binned: every pixel loops over them.
    // The shadow caster is always one of them.
    uint directionalCount = ubo.lightCounts.x;
    int shadowLightIndex = int(ubo.shadowParams.x);
    vec3 directAccum = vec3(0.0);
    vec3 coatAccum = vec3(0.0);
    vec3 coatContribution;
    vec3 sheenAccum = vec3(0.0);
    vec3 sheenContribution;
    for (uint i = 0u; i < directionalCount; ++i)
    {
        vec3 contribution = EvaluateSceneLight(
            sceneLights.lights[i],
            worldPosition,
            N, V,
            albedo, metallic, roughness,
            energyCompensation,
            coat,
            coatContribution,
            sheen,
            sheenContribution,
            anisotropy);
        // Skipped where the light contributes nothing, which includes every surface facing away
        // from it: those are dark already, and the lookup is the most expensive part of the loop.
        // The coat's normal can face the light where the base's does not, so it counts too.
        if (int(i) == shadowLightIndex &&
            (any(greaterThan(contribution, vec3(0.0))) || any(greaterThan(coatContribution, vec3(0.0)))))
        {
            float shadow = EvaluateDirectionalShadow(worldPosition, geoNormal);
            contribution *= shadow;
            coatContribution *= shadow;
            sheenContribution *= shadow;
        }
        directAccum += contribution;
        coatAccum += coatContribution;
        sheenAccum += sheenContribution;
    }

    // Local lights: the pixel's cluster lists every one whose range reaches it, in ascending order,
    // and a light left out contributes exactly zero, so this sums the same values in the same order
    // as the brute-force loop below. That loop stays as the comparison path.
    if (ubo.lightCounts.z != 0u)
    {
        uvec2 range = lightClusters.ranges[FindLightCluster(worldPosition)];
        for (uint k = 0u; k < range.y; ++k)
        {
            SceneLightData light = sceneLights.lights[lightClusters.indices[range.x + k]];
            vec3 contribution = EvaluateSceneLight(
                light,
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution,
                sheen,
                sheenContribution,
                anisotropy);
            ApplyLocalShadow(light, worldPosition, geoNormal, contribution, coatContribution, sheenContribution);
            directAccum += contribution;
            coatAccum += coatContribution;
            sheenAccum += sheenContribution;
        }
    }
    else
    {
        for (uint i = directionalCount; i < ubo.lightCounts.y; ++i)
        {
            SceneLightData light = sceneLights.lights[i];
            vec3 contribution = EvaluateSceneLight(
                light,
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution,
                sheen,
                sheenContribution,
                anisotropy);
            ApplyLocalShadow(light, worldPosition, geoNormal, contribution, coatContribution, sheenContribution);
            directAccum += contribution;
            coatAccum += coatContribution;
            sheenAccum += sheenContribution;
        }
    }

    // The base, emissive included, loses what the coat's Fresnel reflects toward the viewer; the
    // coat adds its own lobe. Uncoated surfaces sum exactly what they did before emissive moved in
    // here: (ambient + direct) + emissive.
    vec3 color;
    if (HasSheen(sheen))
    {
        // The base loses what the sheen lobe reflects, per its albedo at this view; emissive does
        // not pass under the fibres' reflection, as in Filament.
        float sheenScaling = 1.0 - max(sheen.color.r, max(sheen.color.g, sheen.color.b)) *
                                       SampleSheenAlbedo(sheen.roughness, max(dot(N, V), 0.0));
        color = (ambient + directAccum) * sheenScaling + sheenAccum + EvaluateSheenAmbient(N, V, sheen) * ao + emissive;
    }
    else
    {
        color = ambient + directAccum + emissive;
    }
    if (coat.factor > 0.0)
    {
        float coatFresnel = FresnelSchlick(max(dot(coat.normal, V), 0.0), COAT_F0).x;
        vec3 coatAmbient = EvaluateCoatAmbient(coat, V) * ao;
        color = color * (1.0 - coat.factor * coatFresnel) + coat.factor * (coatAmbient + coatAccum);
    }
    return color;
}

#endif
