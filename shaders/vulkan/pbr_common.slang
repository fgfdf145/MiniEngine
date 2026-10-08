#ifndef PBR_COMMON_SLANG
#define PBR_COMMON_SLANG

// SceneLightData, the LIGHT_* and SHADOW_CASCADE_COUNT constants and the ubo block.
#include "scene_common.slang"
#include "ssr_common.slang"
#include "local_shadow_common.slang"
#include "anisotropy_common.slang"
#include "transmission_common.slang"
#include "brdf_common.slang"
#include "ltc_common.slang"
#include "iridescence_common.slang"
// The sky's SH irradiance for the ambient term under a physical sky.
#include "atmosphere_sampling.slang"
#include "spherical_harmonics.slang"
#include "cubemap_common.slang"
// The DDGI probes' irradiance for the diffuse ambient (SceneDiffuseAmbient).
#include "ddgi_common.slang"

// One layer per cascade, sampled with a LESS_OR_EQUAL depth comparison (see VulkanShadowPass).
[[vk::binding(1, 0)]] Texture2DArray<float> shadowMap;
[[vk::binding(FRAME_SAMPLER(1), 0)]] SamplerComparisonState shadowMapSampler;

// Every light the shader evaluates: ubo.lightCounts.x directional lights, then the local ones. The
// C++ side is GpuLightData, written by VulkanUniformBuffer::Update.
[[vk::binding(10, 0)]] StructuredBuffer<SceneLightData> sceneLights;

// The local lights each cluster may be lit by (LightClusterGrid in engine/renderer/light_clusters.h):
// LIGHT_CLUSTER_COUNT ranges (offset, count) into the index list after them, whose entries index
// sceneLights. Read as words, the layout of { uint2 ranges[LIGHT_CLUSTER_COUNT]; uint indices[]; }.
[[vk::binding(11, 0)]] StructuredBuffer<uint> lightClusters;

uint2 LightClusterRange(uint cluster)
{
    return uint2(lightClusters[cluster * 2u], lightClusters[cluster * 2u + 1u]);
}

uint LightClusterIndex(uint index)
{
    return lightClusters[uint(LIGHT_CLUSTER_COUNT) * 2u + index];
}

// The local lights' shadow atlas (see VulkanLocalShadowPass), sampled with a LESS_OR_EQUAL depth
// comparison, and its tiles (GpuLocalShadowTile). A light's areaRightAxis.w is 1 + its first tile,
// 0 when it casts no shadow; a point or area light's six cube faces follow in SelectCubeFace order.
[[vk::binding(13, 0)]] Texture2D<float> localShadowAtlas;
[[vk::binding(FRAME_SAMPLER(13), 0)]] SamplerComparisonState localShadowAtlasSampler;

struct LocalShadowTileData
{
    float4x4 viewProjection;
    float4 atlasRect; // uv offset xy, uv size zw
    float4 params;    // x = world size of one texel at one metre from the light, y = 1 on a cube's tiles
};

[[vk::binding(14, 0)]] StructuredBuffer<LocalShadowTileData> localShadowTiles;

// The area lights' LTC tables (engine/renderer/ltc_table.h): the inverse matrices and the lobe's
// (albedo, Fresnel share), 64 x 64 RGBA32F, x = sqrt(1 - N.V), y = roughness.
[[vk::binding(15, 0)]] Texture2D ltcInverseMatrices;
[[vk::binding(FRAME_SAMPLER(15), 0)]] SamplerState ltcInverseMatricesSampler;
[[vk::binding(16, 0)]] Texture2D ltcAmplitudes;
[[vk::binding(FRAME_SAMPLER(16), 0)]] SamplerState ltcAmplitudesSampler;

// Where (roughness, N.V) falls in the LTC tables, clamped to texel centres.
float2 LtcTableUv(float roughness, float NdV)
{
    const float size = 64.0;
    return clamp(float2(sqrt(1.0 - NdV), roughness), float2(0.5 / size), float2(1.0 - 0.5 / size));
}

static const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR microfacet BRDF helpers
// ---------------------------------------------------------------------------

// The denominator is at least alpha^2 (at the peak), and the 0.04 roughness floor keeps that above
// 2.6e-6, so it needs no clamp beyond keeping it positive. An earlier max(pi d^2, 1e-4) floor cut
// the peak of every lobe smoother than roughness 0.3 or so, by up to five orders of magnitude at
// the floor: smooth surfaces showed a dim blur where the sun should have been.
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = max(NdH2 * (a2 - 1.0) + 1.0, 1e-7);
    return a2 / (PI * denom * denom);
}

// D * G / (4 N.L N.V) of the isotropic GGX lobe: the height-correlated Smith visibility
// (brdf_common.slang), the term the DFG table integrates.
float IsotropicSpecularTerm(float3 N, float3 V, float3 L, float3 H, float roughness)
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
    float3 L;
    float3 toLight;
    float size;
    bool directional;
};

// The direction a lobe around N sees the light along, the point of the source closest to its
// reflection vector, and in normalization the factor its peak falls by as the source spreads it.
float3 SpecularLightDirection(LightSource source, float3 N, float3 V, float roughness, out float normalization)
{
    normalization = 1.0;
    if (source.size <= 0.0)
        return source.L;
    float3 R = reflect(-V, N);
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

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Schlick with an F90 below 1 (KHR_materials_specular).
float3 FresnelSchlick(float cosTheta, float3 F0, float F90)
{
    return F0 + (float3(F90) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------------------------------------------------------------------------
// Dielectric specular (KHR_materials_ior, KHR_materials_specular)
// ---------------------------------------------------------------------------

// The dielectric's reflectance at normal and grazing incidence: 0.04 and 1 for the plain base,
// which is what NoSpecularOverride gives. Metals always reflect their albedo at F0 and 1 at F90;
// SurfaceF0 and SurfaceF90 blend the two by the metallic weight.
struct SpecularParams
{
    float3 dielectricF0;
    float dielectricF90;
    // A thin film over the base (forward pass only): its weight, and its Fresnel at N.V
    // (EvaluateIridescence), which replaces Schlick's by that weight wherever the base's Fresnel
    // is used.
    float iridescenceFactor;
    float3 iridescenceFresnel;
    // KHR_materials_transmission (forward pass only): the share of the dielectric diffuse replaced
    // by transmitted light; the light behind the surface as the transmission copy gave it, already
    // attenuated and tinted (radiance, not pre-exposed); for direct lights, the tint (base colour
    // times the volume's attenuation) and the IOR-scaled GGX alpha of the transmission lobe.
    float transmissionFactor;
    float3 transmittedRadiance;
    float3 transmissionTint;
    float transmissionAlpha;
    // KHR_materials_diffuse_transmission (forward pass only): the share of the diffuse lobe that
    // passes through the surface as a Lambertian lobe on its far side, and that light's colour (the
    // colour factor and map, times the volume's attenuation).
    float diffuseTransmissionFactor;
    float3 diffuseTransmissionColor;
};

SpecularParams NoSpecularOverride()
{
    SpecularParams specular;
    specular.dielectricF0 = float3(0.04);
    specular.dielectricF90 = 1.0;
    specular.iridescenceFactor = 0.0;
    specular.iridescenceFresnel = float3(0.0);
    specular.transmissionFactor = 0.0;
    specular.transmittedRadiance = float3(0.0);
    specular.transmissionTint = float3(0.0);
    specular.transmissionAlpha = 0.0;
    specular.diffuseTransmissionFactor = 0.0;
    specular.diffuseTransmissionColor = float3(0.0);
    return specular;
}

// Whether light passes through the surface at all: its shadow lookups then offset toward a light
// behind it (ShadowOffsetNormal).
bool Transmits(SpecularParams specular)
{
    return specular.transmissionFactor > 0.0 || specular.diffuseTransmissionFactor > 0.0;
}

// The image-based diffuse with diffuse transmission, as the Khronos sample viewer mixes it: the
// irradiance from behind the surface (backIrradiance, for -N) through the transmission colour takes
// the front lobe's place by the factor, under the same Fresnel and metal weights. The caller passes
// it darkened by the ambient occlusion, as the viewer darkens all its image-based light: a translucent
// asset's baked occlusion is authored for it (DiffuseTransmissionTeacup's saucer underside). Specular
// transmission mixes over the result (MixTransmittedAmbient).
float3 MixDiffuseTransmittedAmbient(float3 diffuse, float3 backIrradiance, float metallic, float3 specularAlbedo, SpecularParams specular)
{
    if (specular.diffuseTransmissionFactor <= 0.0)
    {
        return diffuse;
    }
    float3 transmitted = (1.0 - metallic) * (float3(1.0) - specularAlbedo) * specular.diffuseTransmissionColor * backIrradiance;
    return lerp(diffuse, transmitted, specular.diffuseTransmissionFactor);
}

// The image-based diffuse with transmission: the transmitted light takes the diffuse lobe's place
// by the transmission factor, under the same Fresnel and metal weights; the ambient occlusion,
// which is about light reaching the front of the surface, does not darken it.
float3 MixTransmittedAmbient(float3 diffuse, float metallic, float3 specularAlbedo, SpecularParams specular)
{
    if (specular.transmissionFactor <= 0.0)
    {
        return diffuse;
    }
    float3 transmitted = (1.0 - metallic) * (float3(1.0) - specularAlbedo) * specular.transmittedRadiance;
    return lerp(diffuse, transmitted, specular.transmissionFactor);
}

// The base's Fresnel: Schlick's, or the film's by its weight.
float3 BaseFresnel(float cosTheta, float3 F0, float F90, SpecularParams specular)
{
    float3 schlick = FresnelSchlick(cosTheta, F0, F90);
    return specular.iridescenceFactor > 0.0 ? lerp(schlick, specular.iridescenceFresnel, specular.iridescenceFactor) : schlick;
}

// The base lobe's directional albedo for a split-sum pair (A, B) (or the LTC's (norm, fresnel)):
// F0 A + F90 B for Schlick; the film's Fresnel is taken as constant over the lobe, F (A + B).
float3 BaseSpecularAlbedo(float2 ab, float3 F0, float F90, SpecularParams specular)
{
    float3 schlick = F0 * ab.x + F90 * ab.y;
    return specular.iridescenceFactor > 0.0 ? lerp(schlick, specular.iridescenceFresnel * (ab.x + ab.y), specular.iridescenceFactor)
                                            : schlick;
}

float3 SurfaceF0(float3 albedo, float metallic, SpecularParams specular)
{
    return lerp(specular.dielectricF0, albedo, metallic);
}

float SurfaceF90(float metallic, SpecularParams specular)
{
    return lerp(specular.dielectricF90, 1.0, metallic);
}

// What the diffuse lobe keeps: it lives only in the dielectric share of the surface (1 - metallic)
// and under the dielectric's Fresnel alone. Blending the metal's F0 in as well, as SurfaceF0 does
// for the specular lobe, would take a partly metallic surface's metal reflection out of its
// dielectric diffuse a second time. The Khronos Sample Viewer mixes a dielectric BRDF (diffuse
// under the dielectric's Fresnel) and a metal one by the metallic weight; this is that.
float3 DiffuseWeight(float cosTheta, float metallic, SpecularParams specular)
{
    return (float3(1.0) - BaseFresnel(cosTheta, specular.dielectricF0, specular.dielectricF90, specular)) * (1.0 - metallic);
}

// ---------------------------------------------------------------------------
// Anisotropy (KHR_materials_anisotropy)
// ---------------------------------------------------------------------------

// The base's anisotropic stretch: tangent is the world direction the lobe widens along, in the
// surface of the shading normal; strength 0 is the isotropic base, and every anisotropic term below
// is skipped for it, so isotropic surfaces shade exactly as before.
struct AnisotropyParams
{
    float3 tangent;
    float strength;
};

AnisotropyParams NoAnisotropy()
{
    AnisotropyParams anisotropy;
    anisotropy.tangent = float3(1.0, 0.0, 0.0);
    anisotropy.strength = 0.0;
    return anisotropy;
}

// D * G / (4 N.L N.V) of the base's anisotropic lobe (the Khronos sample viewer's):
// alpha_t = lerp(alpha, 1, strength^2) along the tangent, alpha across it.
float AnisotropicSpecularTerm(float3 N, float3 V, float3 L, float3 H, float roughness, AnisotropyParams anisotropy)
{
    float3 T = anisotropy.tangent;
    float3 B = cross(N, T);
    float alpha = roughness * roughness;
    float alphaT = lerp(alpha, 1.0, anisotropy.strength * anisotropy.strength);
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
float3 EvaluateBRDF(
    float3 N, float3 V, float3 L,
    float3 specularL, float specularNormalization,
    float3 albedo, float metallic, float roughness,
    AnisotropyParams anisotropy,
    SpecularParams specularParams,
    float3 energyCompensation,
    float3 radiance)
{
    float NdV = max(dot(N, V), 0.0);
    float3 F0 = SurfaceF0(albedo, metallic, specularParams);
    float F90 = SurfaceF90(metallic, specularParams);
    float3 result = float3(0.0);

    float NdL = max(dot(N, L), 0.0);
    float3 diffuse = float3(0.0);
    if (NdL > 0.0)
    {
        // Burley's diffuse (brdf_common.slang), weighted by what the dielectric's Fresnel toward the
        // centre leaves, as the Lambert term it replaced was.
        float3 H = normalize(V + L);
        float3 kD = DiffuseWeight(max(dot(H, V), 0.0), metallic, specularParams);
        diffuse = kD * albedo * BurleyDiffuse(max(NdV, 1e-4), NdL, max(dot(L, H), 0.0), roughness) * radiance * NdL;
    }
    if (specularParams.diffuseTransmissionFactor > 0.0)
    {
        // Diffuse transmission, as the Khronos sample viewer draws it: the front lobe keeps 1 - factor;
        // a light behind the surface adds a Lambertian lobe through it, in the transmission colour,
        // weighted by the Fresnel about the light mirrored through the surface's plane.
        diffuse *= 1.0 - specularParams.diffuseTransmissionFactor;
        float backNdL = -dot(N, L);
        if (backNdL > 0.0)
        {
            float3 mirrored = normalize(L + 2.0 * N * backNdL);
            float3 H = normalize(mirrored + V);
            float3 kD = DiffuseWeight(max(dot(H, V), 0.0), metallic, specularParams);
            diffuse += specularParams.diffuseTransmissionFactor * kD * specularParams.diffuseTransmissionColor * (backNdL / PI) * radiance;
        }
    }
    if (specularParams.transmissionFactor > 0.0)
    {
        // Light through the surface from behind, as the Khronos sample viewer draws it: the GGX lobe
        // about the light mirrored through the surface's plane, with the IOR-scaled alpha, tinted.
        // It takes the diffuse lobe's place by the transmission factor, under the same weights.
        float3 mirrored = normalize(L + 2.0 * N * dot(-L, N));
        float3 H = normalize(mirrored + V);
        float a2 = specularParams.transmissionAlpha * specularParams.transmissionAlpha;
        float NdH = max(dot(N, H), 0.0);
        float denominator = max(NdH * NdH * (a2 - 1.0) + 1.0, 1e-7);
        float D = a2 / (PI * denominator * denominator);
        float visibility = VisibilitySmithGgxCorrelated(max(NdV, 1e-4), max(dot(N, mirrored), 0.0), specularParams.transmissionAlpha);
        float3 kD = DiffuseWeight(max(NdV, 0.0), metallic, specularParams);
        float3 transmitted = kD * specularParams.transmissionTint * D * visibility * radiance;
        diffuse = lerp(diffuse, transmitted, specularParams.transmissionFactor);
    }
    result += diffuse;

    float specularNdL = max(dot(N, specularL), 0.0);
    if (specularNdL > 0.0)
    {
        float3 H = normalize(V + specularL);
        float3 F = BaseFresnel(max(dot(H, V), 0.0), F0, F90, specularParams);
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
    float3 normal;
};

CoatParams NoCoat()
{
    CoatParams coat;
    coat.factor = 0.0;
    coat.roughness = 1.0;
    coat.normal = float3(0.0, 0.0, 1.0);
    return coat;
}

static const float3 COAT_F0 = float3(0.04);

// The coat lobe's outgoing radiance for the light seen along specularL (SpecularLightDirection for
// the coat's normal and roughness), scaled by that lobe's normalization. Radiance is the light's
// illuminance at normal incidence, as EvaluateBRDF takes it.
float3 EvaluateCoatSpecular(CoatParams coat, float3 V, float3 specularL, float specularNormalization, float3 radiance)
{
    float NdL = max(dot(coat.normal, specularL), 0.0);
    if (NdL <= 0.0)
        return float3(0.0);

    float3 H = normalize(V + specularL);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), COAT_F0);
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
    float3 color;
    // Perceptual roughness, already floored at 0.04.
    float roughness;
};

SheenParams NoSheen()
{
    SheenParams sheen;
    sheen.color = float3(0.0);
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

float3 EvaluateSheen(float3 N, float3 V, float3 L, SheenParams sheen, float3 radiance)
{
    float NdL = max(dot(N, L), 0.0);
    if (NdL <= 0.0)
        return float3(0.0);
    float3 H = normalize(V + L);
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
static const float kMinLightDistanceSquared = 1e-4;

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
float3 AreaLightSpecular(
    float3 worldPos, float3 N, float3 V, float roughness, AnisotropyParams anisotropy, float3 F0, float F90,
    float3 center, float3 lightNormal, float3 rightAxis, float3 upAxis, float2 halfSize, float area,
    out float3 F)
{
    // Representative point: where the reflection ray meets the light's plane, clamped into the
    // rectangle. A ray that never reaches the plane falls back to the receiver's projection onto it.
    float3 R = reflect(-V, N);
    float rayDotNormal = dot(R, lightNormal);
    float3 planePoint = rayDotNormal < -1e-4
                          ? worldPos + R * (dot(center - worldPos, lightNormal) / rayDotNormal)
                          : worldPos - lightNormal * dot(worldPos - center, lightNormal);
    float3 local = planePoint - center;
    float3 closest = center +
                   rightAxis * clamp(dot(local, rightAxis), -halfSize.x, halfSize.x) +
                   upAxis * clamp(dot(local, upAxis), -halfSize.y, halfSize.y);

    float3 toLight = closest - worldPos;
    float lightDistance = max(length(toLight), 0.0001);
    float3 L = toLight / lightDistance;

    float3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    F = FresnelSchlick(max(dot(H, V), 0.0), F0, F90);

    // Moving L to the representative point already spreads the highlight over the light's shape,
    // so the lobe itself is left alone and only renormalized: (alpha / widened)^2, with alpha
    // widened by the half angle of a disc with the rectangle's area. Without it the highlight's
    // peak would stay a point light's while its footprint grew, adding energy.
    float alpha = roughness * roughness;
    float equivalentRadius = sqrt(area / PI);
    float widenedAlpha = clamp(alpha + equivalentRadius / (2.0 * lightDistance), alpha, 1.0);
    float energyNormalization = (alpha * alpha) / (widenedAlpha * widenedAlpha);

    if (NdL <= 0.0)
        return float3(0.0);
    // An anisotropic base stretches the lobe at the same representative point, with the same
    // renormalisation.
    float term = anisotropy.strength > 0.0 ? AnisotropicSpecularTerm(N, V, L, H, roughness, anisotropy)
                                           : IsotropicSpecularTerm(N, V, L, H, roughness);
    return term * energyNormalization * F;
}

// A one-sided Lambertian rectangle, through linearly transformed cosines (Heitz et al. 2016,
// ltc_common.slang, tables fitted to this lobe by tools/ltc_fit): diffuse integrates the clamped
// cosine over the rectangle clipped to the horizon, which is its exact irradiance, and the base's
// and the coat's specular integrate the fitted lobe over it. An anisotropic base, which the
// isotropic fit cannot follow, keeps the representative point of AreaLightSpecular. Sheen, broad
// enough not to care, is evaluated toward the centre.
float3 EvaluateAreaLight(
    SceneLightData light,
    float3 worldPos,
    float3 N, float3 V,
    float3 albedo, float metallic, float roughness,
    float3 energyCompensation,
    CoatParams coat,
    out float3 coatContribution,
    SheenParams sheen,
    out float3 sheenContribution,
    AnisotropyParams anisotropy,
    SpecularParams specularParams)
{
    coatContribution = float3(0.0);
    sheenContribution = float3(0.0);
    float3 center = light.positionAndRange.xyz;
    float3 lightNormal = normalize(light.directionAndType.xyz);
    float3 rightAxis = normalize(light.areaRightAxis.xyz);
    float3 upAxis = cross(lightNormal, rightAxis);
    float2 halfSize = 0.5 * max(light.spotAndArea.zw, float2(0.001));

    // One sided: the back of the rectangle emits nothing.
    if (dot(worldPos - center, lightNormal) <= 0.0)
        return float3(0.0);

    // Relative to the shaded point, which the LTC integration treats as the origin.
    float3 c0 = center - rightAxis * halfSize.x - upAxis * halfSize.y - worldPos;
    float3 c1 = center + rightAxis * halfSize.x - upAxis * halfSize.y - worldPos;
    float3 c2 = center + rightAxis * halfSize.x + upAxis * halfSize.y - worldPos;
    float3 c3 = center - rightAxis * halfSize.x + upAxis * halfSize.y - worldPos;

    // Lumens to luminance for a Lambertian emitter: flux / (pi * area).
    float area = 4.0 * halfSize.x * halfSize.y;
    float3 luminance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (PI * area)) *
                     RangeWindow(distance(worldPos, center), light.positionAndRange.w);

    if (coat.factor > 0.0)
    {
        float2 uv = LtcTableUv(coat.roughness, max(dot(coat.normal, V), 0.0));
        float3x3 minv = LtcInverseMatrix(ltcInverseMatrices.SampleLevel(ltcInverseMatricesSampler, uv, 0.0), coat.normal, V);
        float2 amplitude = ltcAmplitudes.SampleLevel(ltcAmplitudesSampler, uv, 0.0).rg;
        float coverage = LtcIntegrateQuad(mul(minv, c0), mul(minv, c1), mul(minv, c2), mul(minv, c3));
        coatContribution = luminance * (coverage * (COAT_F0 * amplitude.x + (1.0 - COAT_F0) * amplitude.y));
    }

    // The clipped cosine's integral is the form factor over pi; times pi and the luminance it is the
    // irradiance.
    float3x3 cosineFrame = LtcInverseMatrix(float4(1.0, 0.0, 0.0, 1.0), N, V);
    float formFactor = LtcIntegrateQuad(mul(cosineFrame, c0), mul(cosineFrame, c1), mul(cosineFrame, c2), mul(cosineFrame, c3));
    if (formFactor <= 0.0)
        return float3(0.0);
    float3 irradiance = luminance * (PI * formFactor);

    float3 toCentre = normalize(center - worldPos);
    float3 centreH = normalize(V + toCentre);
    float NdV = max(dot(N, V), 1e-4);
    if (HasSheen(sheen))
    {
        sheenContribution = sheen.color *
                            (DistributionCharlie(sheen.roughness, max(dot(N, centreH), 0.0)) *
                             VisibilityNeubelt(NdV, max(dot(N, toCentre), 1e-4))) *
                            irradiance;
    }

    float3 F0 = SurfaceF0(albedo, metallic, specularParams);
    float F90 = SurfaceF90(metallic, specularParams);
    float3 specular;
    if (anisotropy.strength > 0.0)
    {
        float3 F;
        specular = AreaLightSpecular(
                       worldPos, N, V, roughness, anisotropy, F0, F90,
                       center, lightNormal, rightAxis, upAxis, halfSize, area,
                       F) *
                   irradiance;
    }
    else
    {
        float2 uv = LtcTableUv(roughness, NdV);
        float3x3 minv = LtcInverseMatrix(ltcInverseMatrices.SampleLevel(ltcInverseMatricesSampler, uv, 0.0), N, V);
        float2 amplitude = ltcAmplitudes.SampleLevel(ltcAmplitudesSampler, uv, 0.0).rg;
        float coverage = LtcIntegrateQuad(mul(minv, c0), mul(minv, c1), mul(minv, c2), mul(minv, c3));
        // F0 norm + (F90 - F0) fresnel, as the split-sum pair (norm - fresnel, fresnel).
        specular = luminance * coverage * BaseSpecularAlbedo(float2(amplitude.x - amplitude.y, amplitude.y), F0, F90, specularParams);
    }
    specular *= energyCompensation;

    // Burley's diffuse, weighted by the Fresnel toward the centre, applied to the exact irradiance.
    float3 kD = DiffuseWeight(max(dot(V, centreH), 0.0), metallic, specularParams);
    float3 diffuse = kD * albedo * BurleyDiffuse(NdV, clamp(dot(N, toCentre), 1e-4, 1.0), max(dot(toCentre, centreH), 0.0), roughness) * irradiance;

    return diffuse + specular;
}

// ---------------------------------------------------------------------------
// Directional shadow
// ---------------------------------------------------------------------------

// How far, in texels of the cascade in use, the lookup moves off the surface along its geometric
// normal. Moving the receiver rather than biasing its depth keeps acne off surfaces at grazing
// angles to the light without detaching shadows from the casters' feet ("peter panning").
static const float kShadowNormalOffsetTexels = 1.5;

// The last fraction of each cascade's depth range fades into the next cascade, or into no shadow
// after the last one, so neither the cascade switch nor the shadow distance shows as a hard line.
static const float kShadowCascadeBlendFraction = 0.1;

// 3x3 taps, each a bilinear comparison: an even 4x4 texel filter.
float SampleShadowCascade(int cascade, float3 worldPos, float3 geoNormal)
{
    float3 offsetPos = worldPos + geoNormal * (ubo.shadowCascadeTexelSizes[cascade] * kShadowNormalOffsetTexels);
    float4 lightClip = mul(ubo.shadowCascadeViewProjection[cascade], float4(offsetPos, 1.0));
    float2 uv = lightClip.xy * 0.5 + 0.5;
    float texel = ubo.shadowParams.y;

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            // An explicit level: the lookup runs in non-uniform control flow, where implicit
            // derivatives are undefined, and the map has a single mip level anyway.
            lit += shadowMap.SampleCmpLevelZero(shadowMapSampler, float3(uv + float2(x, y) * texel, float(cascade)), lightClip.z);
        }
    }
    return lit / 9.0;
}

// The ray traced sun shadow at the pixel being shaded (rt_shadow_filter.comp), which the deferred
// lighting pass sets before ShadeSurface when it runs; negative, the cascades answer.
static float tracedSunShadow = -1.0;

// The fraction of the shadow casting light that reaches this point: 1 lit, 0 in shadow. The
// clouds' shadow (cloud_shadow.slang) multiplies the cascades' and reaches beyond them.
float EvaluateDirectionalShadow(float3 worldPos, float3 geoNormal)
{
    if (tracedSunShadow >= 0.0)
    {
        return tracedSunShadow * CloudShadow(worldPos);
    }
    float viewDepth = -mul(ubo.view, float4(worldPos, 1.0)).z;
    int cascade = -1;
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i)
    {
        if (viewDepth <= ubo.shadowCascadeSplits[i])
        {
            cascade = i;
            break;
        }
    }
    float clouds = CloudShadow(worldPos);
    if (cascade < 0)
        return clouds;

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
        lit = lerp(lit, nextLit, blend);
    }
    return lit * clouds;
}

// ---------------------------------------------------------------------------
// Local light shadows
// ---------------------------------------------------------------------------

// The fraction of a local light that reaches this point: 1 lit, 0 in shadow. The light must have a
// tile (areaRightAxis.w > 0).
float EvaluateLocalShadow(SceneLightData light, float3 worldPos, float3 geoNormal)
{
    float3 fromLight = worldPos - light.positionAndRange.xyz;
    int tileIndex = int(light.areaRightAxis.w) - 1;
    // The planner marks the first tile of a cube; a spot light may have been given one too.
    if (localShadowTiles[tileIndex].params.y > 0.5)
        tileIndex += SelectCubeFace(fromLight);
    LocalShadowTileData tile = localShadowTiles[tileIndex];

    // The cascades' normal offset, with a texel that grows with the distance to the light.
    float texel = tile.params.x * max(length(fromLight), kLocalShadowNearPlaneMetres);
    float3 offsetPos = worldPos + geoNormal * (texel * kShadowNormalOffsetTexels);
    float3 coords = LocalShadowAtlasCoordinates(mul(tile.viewProjection, float4(offsetPos, 1.0)), tile.atlasRect, kLocalShadowGuardFraction);

    float atlasTexel = tile.atlasRect.z / kLocalShadowTileTexels;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            // An explicit level for the same reason as the cascades: non-uniform control flow.
            lit += localShadowAtlas.SampleCmpLevelZero(localShadowAtlasSampler, coords.xy + float2(x, y) * atlasTexel, coords.z);
        }
    }
    return lit / 9.0;
}

// ---------------------------------------------------------------------------
// Uniform ambient
// ---------------------------------------------------------------------------

// The DFG table at (roughness, N.V), clamped to texel centres so the lookup never wraps.
float2 SampleEnvironmentBrdf(float roughness, float NdV)
{
    const float size = 64.0;
    float2 uv = clamp(float2(NdV, roughness), float2(0.5 / size), float2(1.0 - 0.5 / size));
    return environmentBrdfLut.SampleLevel(environmentBrdfLutSampler, uv, 0.0).rg;
}

// Scales a single-scattering GGX specular term up by the energy the lobe loses to light bouncing
// between microfacets more than once (Fdez-Aguera 2019, as the Khronos Sample Viewer's
// getIBLGGXFresnel has it): 1 + E_ms F_avg / (1 - F_avg E_ms) with E_ms = 1 - (A + B) and F_avg the
// average Fresnel. environmentBrdf is the (A, B) pair the specular term was computed with: A + B is
// the lobe's directional albedo for F0 = 1, so a perfect conductor then reflects exactly all it
// receives. Rough metals gain the most; dielectrics, with their small F0, barely change. SpecularEnergyCompensation in
// engine/renderer/environment_brdf.cpp is the same formula and carries the tests.
float3 SpecularEnergyCompensation(float3 F0, float2 environmentBrdf)
{
    float lostEnergy = clamp(1.0 - (environmentBrdf.x + environmentBrdf.y), 0.0, 1.0 - 1e-4);
    float3 averageFresnel = F0 + (float3(1.0) - F0) / 21.0;
    return float3(1.0) + lostEnergy * averageFresnel / (float3(1.0) - averageFresnel * lostEnergy);
}

// The base's split-sum specular albedo with its multiple scattering, and the dielectric share's alone
// (what the diffuse lobe sits under, see DiffuseWeight). Each of the two lobes, the dielectric's
// and the metal's, is compensated for its own F0 and then blended by the metallic weight, as the
// Khronos Sample Viewer mixes them; compensating the blended F0 would not be the same, the factor
// being non-linear in F0.
struct BaseSpecularAlbedos
{
    float3 total;
    float3 dielectric;
};

BaseSpecularAlbedos EvaluateBaseSpecularAlbedos(float2 environmentBrdf, float3 albedo, float metallic, SpecularParams specular)
{
    BaseSpecularAlbedos albedos;
    albedos.dielectric = BaseSpecularAlbedo(environmentBrdf, specular.dielectricF0, specular.dielectricF90, specular) *
                         SpecularEnergyCompensation(specular.dielectricF0, environmentBrdf);
    float3 metal = metallic > 0.0 ? BaseSpecularAlbedo(environmentBrdf, albedo, 1.0, specular) * SpecularEnergyCompensation(albedo, environmentBrdf)
                                : float3(0.0);
    albedos.total = lerp(albedos.dielectric, metal, metallic);
    return albedos;
}

// Outgoing radiance from the scene's Ambient and Hemisphere lights (SceneAmbientAlong). For a uniform
// environment the split sum is exact apart from the BRDF fit: the prefiltered radiance is the
// luminance itself whatever the roughness. Metals get only the specular lobe, tinted by their F0,
// and the diffuse lobe keeps whatever energy the specular one did not reflect.
// The specular lobe's incoming radiance: the environment's, occluded by the AO as a lobe of this
// roughness is (Lagarde 2014), replaced by the screen-space reflection as far as it is trusted.
// reflection.rgb is physical radiance, reflection.a its confidence.
// horizon is HorizonSpecularOcclusion for the reflection vector: the environment below the
// geometric surface is blocked by it. Screen-space reflections see the real geometry and are not
// faded.
float3 SpecularAmbientRadiance(float3 environment, float4 reflection, float NdV, float ao, float roughness, float horizon)
{
    return lerp(environment * (SpecularOcclusion(NdV, ao, roughness) * horizon), reflection.rgb, reflection.a);
}

// Defined after EvaluateSkyIrradiance, which it falls back on.
float3 SceneDiffuseAmbient(float3 worldPosition, float3 n, float3 V);
float3 SceneSpecularEnvironment(float3 worldPosition, float3 N, float3 R, float3 V, float3 environment);

float3 EvaluateUniformAmbient(
    float3 worldPosition, float3 N, float3 geoNormal, float3 V,
    float3 albedo, float metallic, float roughness,
    SpecularParams specular,
    float ao,
    float4 reflection)
{
    float NdV = max(dot(N, V), 0.0);
    // The DFG table, as under a physical sky: Karis' analytic fit, used here before, approximated the
    // Smith-Schlick table and no longer matches the correlated lobe the direct lights draw.
    float2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    // Compensated with the same table it was computed from, so the white furnace holds.
    BaseSpecularAlbedos albedos = EvaluateBaseSpecularAlbedos(environmentBrdf, albedo, metallic, specular);
    float3 specularAlbedo = albedos.total;
    float3 diffuseAlbedo = albedo * (1.0 - metallic) * (float3(1.0) - albedos.dielectric);
    float3 R = reflect(-V, N);
    float horizon = HorizonSpecularOcclusion(R, geoNormal);
    float3 diffuse = MixDiffuseTransmittedAmbient(
        diffuseAlbedo * SceneDiffuseAmbient(worldPosition, N, V) * ao,
        specular.diffuseTransmissionFactor > 0.0 ? SceneDiffuseAmbient(worldPosition, -N, V) * ao : float3(0.0),
        metallic, albedos.dielectric, specular);
    diffuse = MixTransmittedAmbient(diffuse, metallic, albedos.dielectric, specular);
    float3 environment = SceneSpecularEnvironment(worldPosition, N, R, V, SceneAmbientAlong(R));
    return diffuse + specularAlbedo * SpecularAmbientRadiance(environment, reflection, NdV, ao, roughness, horizon);
}

// ---------------------------------------------------------------------------
// Per-light contribution
// ---------------------------------------------------------------------------

// What a directional, point or spot light sends to worldPos: the direction toward it, the irradiance
// on a surface facing it (lux) and its source for the specular lobe. False for the other types
// (ambient, which is folded into ambientLuminance, and area lights, which integrate over their
// rectangle instead).
bool PunctualIncidence(SceneLightData light, float3 worldPos, out float3 L, out float3 radiance, out LightSource source)
{
    int lightType = int(light.directionAndType.w);
    source.toLight = float3(0.0);
    source.size = 0.0;
    source.directional = false;
    L = float3(0.0, 0.0, 1.0);
    radiance = float3(0.0);

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
        float3 toLight = light.positionAndRange.xyz - worldPos;
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
        float3 toLight = light.positionAndRange.xyz - worldPos;
        source.toLight = toLight;
        source.size = light.spotAndArea.z;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);

        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);

        float3 spotDir = normalize(light.directionAndType.xyz);
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
    else
    {
        return false;
    }
    source.L = L;
    return true;
}

float3 EvaluateSceneLight(
    SceneLightData light,
    float3 worldPos,
    float3 N, float3 V,
    float3 albedo, float metallic, float roughness,
    float3 energyCompensation,
    CoatParams coat,
    out float3 coatContribution,
    SheenParams sheen,
    out float3 sheenContribution,
    AnisotropyParams anisotropy,
    SpecularParams specularParams)
{
    coatContribution = float3(0.0);
    sheenContribution = float3(0.0);

    if (int(light.directionAndType.w) == LIGHT_AREA)
    {
        // Integrates over the rectangle itself, so it does not go through EvaluateBRDF.
        return EvaluateAreaLight(
            light, worldPos, N, V, albedo, metallic, roughness, energyCompensation,
            coat, coatContribution, sheen, sheenContribution, anisotropy, specularParams);
    }
    float3 L;
    float3 radiance;
    LightSource source;
    if (!PunctualIncidence(light, worldPos, L, radiance, source))
    {
        return float3(0.0); // an ambient light is part of the ambient term
    }

    if (coat.factor > 0.0)
    {
        float coatNormalization;
        float3 coatL = SpecularLightDirection(source, coat.normal, V, coat.roughness, coatNormalization);
        coatContribution = EvaluateCoatSpecular(coat, V, coatL, coatNormalization, radiance);
    }
    if (HasSheen(sheen))
    {
        sheenContribution = EvaluateSheen(N, V, L, sheen, radiance);
    }
    float specularNormalization;
    float3 specularL = SpecularLightDirection(source, N, V, roughness, specularNormalization);
    return EvaluateBRDF(N, V, L, specularL, specularNormalization, albedo, metallic, roughness, anisotropy, specularParams, energyCompensation, radiance);
}

// Sky irradiance for a direction from the active sky's SH: the atmosphere's (computed on the GPU)
// or the HDRI's (projected on the CPU, in the camera block).
float3 EvaluateSkyIrradiance(float3 direction)
{
    float basis[9];
    EvaluateShBasis(direction, basis);
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    float3 irradiance = float3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        float3 coefficient = hdri ? ubo.hdriIrradianceSh[i].rgb : skyIrradiance[0].coefficients[i].rgb;
        irradiance += coefficient * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, float3(0.0));
}

// The share of the DDGI probes' light that reaches the surface being shaded where a level coarser
// than the finest answers: the ray traced probe occlusion (rt_occlusion.comp, SceneAo's g). The
// deferred lighting pass sets it per pixel before ShadeSurface; everywhere else the probes' light
// stands as it is.
static float ddgiProbeOcclusion = 1.0;

// The light the diffuse ambient term receives at worldPosition from around n, as irradiance / pi (the
// ambient luminance's unit): the DDGI probes where they reach, which hold the sky, the Ambient and
// Hemisphere lights and every bounce off the scene; the sky's SH and the ambient lights where they do
// not, blended over the volume's outer cells. V points toward the viewer (the probes' view bias).
float3 SceneDiffuseAmbient(float3 worldPosition, float3 n, float3 V)
{
    float3 fallback = EnvironmentMode() == ENVIRONMENT_NONE
                        ? SceneAmbientAlong(n)
                        : EvaluateSkyIrradiance(n) / ATMOSPHERE_PI + SceneLightsAmbientAlong(n);
    if (DdgiLevelCount() == 0u)
    {
        return fallback;
    }
    float weight;
    float3 probes = DdgiIrradiance(worldPosition, n, V, weight).rgb * ddgiProbeOcclusion;
    return probes + (1.0 - weight) * fallback;
}

// What a specular lobe along R receives, given what the sky and the ambient lights send along it
// (environment): inside the DDGI volume, only the share of it the probes see escape around R; the
// rest comes from the surroundings, whose average radiance is what the probes gathered around R
// minus what of it came from the sky. Under a roof or in a tunnel the sky's reflection gives way to
// the walls' light. Where the screen-space reflection is trusted it replaces both, as before.
float3 SceneSpecularEnvironment(float3 worldPosition, float3 N, float3 R, float3 V, float3 environment)
{
    if (DdgiLevelCount() == 0u)
    {
        return environment;
    }
    float weight;
    float4 probes = DdgiIrradianceAlong(worldPosition, N, V, R, weight);
    if (weight <= 1e-4)
    {
        return environment;
    }
    probes *= ddgiProbeOcclusion / weight;
    float skyVisibility = clamp(probes.a, 0.0, 1.0);
    // The probes' irradiance / pi around R is the sky's, times the share of rays that escaped, plus
    // the surroundings', times the rest.
    float3 skyAround = EnvironmentMode() == ENVIRONMENT_NONE
                         ? SceneAmbientAlong(R)
                         : EvaluateSkyIrradiance(R) / ATMOSPHERE_PI + SceneLightsAmbientAlong(R);
    float3 surroundings = max(probes.rgb - skyVisibility * skyAround, float3(0.0));
    return lerp(environment, environment * skyVisibility + surroundings, weight);
}

// The ambient term under a physical sky, split-sum (Karis 2013): the diffuse lobe sees the SH
// irradiance for N, the specular lobe the GGX-prefiltered sky along R at the surface's roughness,
// weighted by the DFG table's F0 A + B. The scene's Ambient and Hemisphere lights, but not the
// fallback, add theirs (SceneLightsAmbientAlong).
float3 EvaluateSkyAmbient(float3 worldPosition, float3 N, float3 geoNormal, float3 V, float3 albedo, float metallic, float roughness, AnisotropyParams anisotropy, SpecularParams specular, float ao, float4 reflection)
{
    float NdV = max(dot(N, V), 0.0);
    float2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    BaseSpecularAlbedos albedos = EvaluateBaseSpecularAlbedos(environmentBrdf, albedo, metallic, specular);
    float3 specularAlbedo = albedos.total;
    float3 diffuseAlbedo = albedo * (1.0 - metallic) * (float3(1.0) - albedos.dielectric);
    // An anisotropic base reflects about the bent normal (AnisotropicBentNormal); the DFG weight
    // keeps the isotropic roughness.
    float3 R = reflect(-V, anisotropy.strength > 0.0 ? AnisotropicBentNormal(N, V, anisotropy.tangent, anisotropy.strength, roughness) : N);
    float3 environment = SceneSpecularEnvironment(
        worldPosition, N, R, V,
        prefilteredEnvironment.SampleLevel(prefilteredEnvironmentSampler, R, roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb + SceneLightsAmbientAlong(R));
    float3 diffuse = diffuseAlbedo * SceneDiffuseAmbient(worldPosition, N, V) * ao;
    if (specular.diffuseTransmissionFactor > 0.0)
    {
        diffuse = MixDiffuseTransmittedAmbient(diffuse, SceneDiffuseAmbient(worldPosition, -N, V) * ao, metallic, albedos.dielectric, specular);
    }
    diffuse = MixTransmittedAmbient(diffuse, metallic, albedos.dielectric, specular);
    return diffuse + specularAlbedo * SpecularAmbientRadiance(environment, reflection, NdV, ao, roughness, HorizonSpecularOcclusion(R, geoNormal));
}

// ---------------------------------------------------------------------------
// Light clusters
// ---------------------------------------------------------------------------

// The cluster a world position falls in: its tile from the NDC the camera projects it to, its slice
// from its view depth. FindLightCluster in engine/renderer/light_clusters.cpp is the same arithmetic,
// and the CPU binned the lights through the same projection, Y flip included, so the two agree on
// which tile a pixel is in.
uint FindLightCluster(float3 worldPosition)
{
    float4 viewPosition = mul(ubo.view, float4(worldPosition, 1.0));
    float4 clip = mul(ubo.proj, viewPosition);
    float2 ndc = clip.xy / clip.w;
    float2 tileCount = float2(LIGHT_CLUSTER_TILES_X, LIGHT_CLUSTER_TILES_Y);
    uint2 tile = uint2(clamp(floor((ndc * 0.5 + 0.5) * tileCount), float2(0.0), tileCount - 1.0));
    float slice = floor(log(max(-viewPosition.z, 1e-4)) * ubo.lightClusterSlices.x + ubo.lightClusterSlices.y);
    uint sliceIndex = uint(clamp(slice, 0.0, float(LIGHT_CLUSTER_SLICES - 1)));
    return (sliceIndex * LIGHT_CLUSTER_TILES_Y + tile.y) * LIGHT_CLUSTER_TILES_X + tile.x;
}

// The sheen lobe's directional albedo, IntegrateSheenAlbedo clamped to 1, from the DFG table's
// blue channel.
float SampleSheenAlbedo(float roughness, float NdV)
{
    const float size = 64.0;
    float2 uv = clamp(float2(NdV, roughness), float2(0.5 / size), float2(1.0 - 0.5 / size));
    return environmentBrdfLut.SampleLevel(environmentBrdfLutSampler, uv, 0.0).b;
}

// The sheen's share of the ambient term, as Filament shades it: the GGX-prefiltered sky along the
// reflection at the sheen's roughness (the uniform ambient under None), times the sheen colour and
// its albedo. The caller applies the occlusion.
float3 EvaluateSheenAmbient(float3 worldPosition, float3 N, float3 V, SheenParams sheen)
{
    float albedo = SampleSheenAlbedo(sheen.roughness, max(dot(N, V), 0.0));
    if (EnvironmentMode() == ENVIRONMENT_NONE)
    {
        float3 uniformR = reflect(-V, N);
        return sheen.color * albedo * SceneSpecularEnvironment(worldPosition, N, uniformR, V, SceneAmbientAlong(uniformR));
    }
    float3 R = reflect(-V, N);
    float3 sky = prefilteredEnvironment.SampleLevel(prefilteredEnvironmentSampler, R, sheen.roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    return sheen.color * albedo * SceneSpecularEnvironment(worldPosition, N, R, V, sky + SceneLightsAmbientAlong(R));
}

// The coat's share of the ambient term: its lobe's directional albedo (0.04 A + B) times what the
// environment sends along the coat's reflection, the same split sum the base's specular uses. Under
// the uniform ambient that is the ambient luminance itself, weighted by the table as the base is.
// The environment is occluded as the base's is (SpecularAmbientRadiance: the AO through
// SpecularOcclusion at the coat's roughness, the geometric horizon), and the screen-space
// reflection replaces it where the trace followed the coat (SsrTracesCoat); otherwise reflection
// is zero. On car paint the coat is the mirror lobe: without this it reflected the sky where the
// road and the scene around the car should be.
float3 EvaluateCoatAmbient(float3 worldPosition, CoatParams coat, float3 geoNormal, float3 V, float ao, float4 reflection)
{
    float NdV = max(dot(coat.normal, V), 0.0);
    float2 environmentBrdf = SampleEnvironmentBrdf(coat.roughness, NdV);
    float3 coatAlbedo = COAT_F0 * environmentBrdf.x + environmentBrdf.y;
    float3 R = reflect(-V, coat.normal);
    float3 environment = EnvironmentMode() == ENVIRONMENT_NONE
                           ? SceneSpecularEnvironment(worldPosition, coat.normal, R, V, SceneAmbientAlong(R))
                           : SceneSpecularEnvironment(
                                 worldPosition, coat.normal, R, V,
                                 prefilteredEnvironment.SampleLevel(prefilteredEnvironmentSampler, R, coat.roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb + SceneLightsAmbientAlong(R));
    return coatAlbedo * SpecularAmbientRadiance(environment, reflection, NdV, ao, coat.roughness, HorizonSpecularOcclusion(R, geoNormal));
}

#ifdef LOCAL_SHADOW_RAYS
// The ray traced local light shadow (deferred_lighting.frag's ray query variant), used instead of the
// atlas while tracedLocalShadows is set.
float TraceLocalLightShadow(SceneLightData light, float3 worldPosition, float3 offsetNormal);
static bool tracedLocalShadows = false;
#endif

// Darkens a local light's contributions by its shadow, where it has a tile and lights anything:
// like the directional caster, the lookup is skipped where the light contributes nothing.
// A surface that transmits offsets its lookup toward a light behind it (ShadowOffsetNormal).
void ApplyLocalShadow(
    SceneLightData light, float3 worldPosition, float3 geoNormal, bool transmits,
    inout float3 contribution, inout float3 coatContribution, inout float3 sheenContribution)
{
    if (light.areaRightAxis.w > 0.5 &&
        (any(contribution > float3(0.0)) || any(coatContribution > float3(0.0))))
    {
        float3 toLight = light.positionAndRange.xyz - worldPosition;
#ifdef LOCAL_SHADOW_RAYS
        float shadow = tracedLocalShadows ? TraceLocalLightShadow(light, worldPosition, ShadowOffsetNormal(geoNormal, toLight, transmits))
                                          : EvaluateLocalShadow(light, worldPosition, ShadowOffsetNormal(geoNormal, toLight, transmits));
#else
        float shadow = EvaluateLocalShadow(light, worldPosition, ShadowOffsetNormal(geoNormal, toLight, transmits));
#endif
        contribution *= shadow;
        coatContribution *= shadow;
        sheenContribution *= shadow;
    }
}

// Loops over every local light instead of the cluster grid's list: for a point outside the view
// frustum the grid covers (what a ray traced reflection hits).
static bool shadeAllLocalLights = false;

// ---------------------------------------------------------------------------
// Path traced indirect light
// ---------------------------------------------------------------------------

// The lobes the path tracer (path_trace.comp) samples at a G-buffer surface, each as its directional
// albedo under the layers above it: the base's diffuse and specular (the split sum's, energy
// compensated, as the ambient term weights them) under the sheen's and the coat's Fresnel, and the
// coat's own. The trace divides the light each brings by these and the lighting pass multiplies it
// back, so the denoiser filters light rather than texture, and the two cannot disagree. The specular
// and coat lobes share one channel. Anisotropy and the sheen's own lobe are left out.
struct PathTraceLobes
{
    float3 diffuse;
    float3 specular;
    float3 coat;
    // The base's lobes are scaled by this (what the sheen and the coat leave of them), and its
    // specular's energy compensation over the single-scattering lobe the trace samples.
    float baseScale;
    float3 energyCompensation;
};

PathTraceLobes PathTraceLobesOf(
    float3 N, float3 V, float3 albedo, float metallic, float roughness, SpecularParams specular, CoatParams coat, SheenParams sheen)
{
    float NdV = max(dot(N, V), 0.0);
    float2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    BaseSpecularAlbedos albedos = EvaluateBaseSpecularAlbedos(environmentBrdf, albedo, metallic, specular);
    float3 uncompensated = BaseSpecularAlbedo(environmentBrdf, SurfaceF0(albedo, metallic, specular), SurfaceF90(metallic, specular), specular);

    PathTraceLobes lobes;
    lobes.baseScale = 1.0;
    if (HasSheen(sheen))
    {
        lobes.baseScale = 1.0 - max(sheen.color.r, max(sheen.color.g, sheen.color.b)) * SampleSheenAlbedo(sheen.roughness, NdV);
    }
    lobes.coat = float3(0.0);
    if (coat.factor > 0.0)
    {
        float coatNdV = max(dot(coat.normal, V), 0.0);
        lobes.baseScale *= 1.0 - coat.factor * FresnelSchlick(coatNdV, COAT_F0).x;
        float2 coatBrdf = SampleEnvironmentBrdf(coat.roughness, coatNdV);
        lobes.coat = float3(coat.factor * (COAT_F0.x * coatBrdf.x + coatBrdf.y));
    }
    lobes.diffuse = albedo * (1.0 - metallic) * (float3(1.0) - albedos.dielectric) * lobes.baseScale;
    lobes.specular = albedos.total * lobes.baseScale;
    lobes.energyCompensation = albedos.total / max(uncompensated, float3(1e-4));
    return lobes;
}

// Set by the deferred lighting pass in path tracing mode: the path traced light reaching the surface
// through its diffuse and specular lobes, demodulated (physical radiance per unit lobe albedo), which
// takes the place of every ambient term ShadeSurface would add.
static bool pathTracedIndirect = false;
static float3 pathTracedDiffuse = float3(0.0);
static float3 pathTracedSpecular = float3(0.0);

// Ambient plus every direct light for one resolved surface point; the caller adds emissive. The
// arithmetic and its order are exactly what triangle.frag's main() ran inline before phase two,
// (ambient + direct) with emissive added afterwards, so the forward image is unchanged by the move
// and the deferred path shares it by construction rather than by copy.
//
// N is the shading normal, normal map applied. geoNormal is the interpolated vertex normal,
// flipped for back faces the way triangle.frag flips it: the shadow lookup offsets along it, not
// along N, so the shadow boundary does not follow the normal map.
float3 ShadeSurface(
    float3 worldPosition, float3 N, float3 geoNormal, float3 V,
    float3 albedo, float metallic, float roughness, float ao,
    float3 emissive,
    CoatParams coat,
    SheenParams sheen,
    AnisotropyParams anisotropy,
    SpecularParams specular,
    float4 reflection)
{
    // The CPU has already summed the scene's Ambient lights into ambientLuminance, or put the
    // fallback there when there are none; see SelectSceneLights. Under a physical sky the sky's
    // irradiance takes the fallback's place.
    // The AO darkens the diffuse lobe directly and the specular one through SpecularOcclusion; a
    // screen-space reflection (reflection.a > 0) replaces the occluded environment where it is trusted.
    // The reflection was traced for the coat's lobe where SsrTracesCoat says so; the base then sees
    // the environment alone.
    bool coatTakesReflection = SsrTracesCoat(coat.factor, coat.roughness, roughness);
    float4 baseReflection = coatTakesReflection ? float4(0.0) : reflection;
    // In path tracing mode the traced light replaces all of it, added once the layers are composed.
    float3 ambient = pathTracedIndirect ? float3(0.0)
                 : EnvironmentMode() == ENVIRONMENT_NONE
                       ? EvaluateUniformAmbient(worldPosition, N, geoNormal, V, albedo, metallic, roughness, specular, ao, baseReflection)
                       : EvaluateSkyAmbient(worldPosition, N, geoNormal, V, albedo, metallic, roughness, anisotropy, specular, ao, baseReflection);

    // One factor for every direct light: it depends only on the surface and the view. It comes
    // from the DFG table, which integrates the same height-correlated lobe the direct lights draw,
    // so it adds back exactly the energy that lobe loses.
    // Per lobe, as EvaluateBaseSpecularAlbedos compensates them: the blended lobe's factor is the
    // two compensated albedos' blend over the uncompensated blended albedo.
    float2 directEnvironmentBrdf = SampleEnvironmentBrdf(roughness, max(dot(N, V), 0.0));
    float3 uncompensated = BaseSpecularAlbedo(directEnvironmentBrdf, SurfaceF0(albedo, metallic, specular), SurfaceF90(metallic, specular), specular);
    float3 energyCompensation = EvaluateBaseSpecularAlbedos(directEnvironmentBrdf, albedo, metallic, specular).total / max(uncompensated, float3(1e-4));

    // Directional lights reach everywhere, so they are never binned: every pixel loops over them.
    // The shadow caster is always one of them.
    uint directionalCount = ubo.lightCounts.x;
    int shadowLightIndex = int(ubo.shadowParams.x);
    float3 directAccum = float3(0.0);
    float3 coatAccum = float3(0.0);
    float3 coatContribution;
    float3 sheenAccum = float3(0.0);
    float3 sheenContribution;
    for (uint i = 0u; i < directionalCount; ++i)
    {
        float3 contribution = EvaluateSceneLight(
            sceneLights[i],
            worldPosition,
            N, V,
            albedo, metallic, roughness,
            energyCompensation,
            coat,
            coatContribution,
            sheen,
            sheenContribution,
            anisotropy,
            specular);
        // Skipped where the light contributes nothing, which includes every surface facing away
        // from it: those are dark already, and the lookup is the most expensive part of the loop.
        // The coat's normal can face the light where the base's does not, so it counts too.
        if (int(i) == shadowLightIndex &&
            (any(contribution > float3(0.0)) || any(coatContribution > float3(0.0))))
        {
            float shadow = EvaluateDirectionalShadow(
                worldPosition, ShadowOffsetNormal(geoNormal, -sceneLights[i].directionAndType.xyz, Transmits(specular)));
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
    if (ubo.lightCounts.z != 0u && !shadeAllLocalLights)
    {
        uint2 range = LightClusterRange(FindLightCluster(worldPosition));
        for (uint k = 0u; k < range.y; ++k)
        {
            SceneLightData light = sceneLights[LightClusterIndex(range.x + k)];
            float3 contribution = EvaluateSceneLight(
                light,
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution,
                sheen,
                sheenContribution,
                anisotropy,
                specular);
            ApplyLocalShadow(light, worldPosition, geoNormal, Transmits(specular), contribution, coatContribution, sheenContribution);
            directAccum += contribution;
            coatAccum += coatContribution;
            sheenAccum += sheenContribution;
        }
    }
    else
    {
        for (uint i = directionalCount; i < ubo.lightCounts.y; ++i)
        {
            SceneLightData light = sceneLights[i];
            float3 contribution = EvaluateSceneLight(
                light,
                worldPosition,
                N, V,
                albedo, metallic, roughness,
                energyCompensation,
                coat,
                coatContribution,
                sheen,
                sheenContribution,
                anisotropy,
                specular);
            ApplyLocalShadow(light, worldPosition, geoNormal, Transmits(specular), contribution, coatContribution, sheenContribution);
            directAccum += contribution;
            coatAccum += coatContribution;
            sheenAccum += sheenContribution;
        }
    }

    // The base, emissive included, loses what the coat's Fresnel reflects toward the viewer; the
    // coat adds its own lobe. Uncoated surfaces sum exactly what they did before emissive moved in
    // here: (ambient + direct) + emissive.
    float3 color;
    if (HasSheen(sheen))
    {
        // The base loses what the sheen lobe reflects, per its albedo at this view; emissive does
        // not pass under the fibres' reflection, as in Filament.
        float sheenScaling = 1.0 - max(sheen.color.r, max(sheen.color.g, sheen.color.b)) *
                                       SampleSheenAlbedo(sheen.roughness, max(dot(N, V), 0.0));
        float3 sheenAmbient = pathTracedIndirect ? float3(0.0) : EvaluateSheenAmbient(worldPosition, N, V, sheen) * ao;
        color = (ambient + directAccum) * sheenScaling + sheenAccum + sheenAmbient + emissive;
    }
    else
    {
        color = ambient + directAccum + emissive;
    }
    if (coat.factor > 0.0)
    {
        float coatFresnel = FresnelSchlick(max(dot(coat.normal, V), 0.0), COAT_F0).x;
        float3 coatAmbient = pathTracedIndirect ? float3(0.0)
                                              : EvaluateCoatAmbient(worldPosition, coat, geoNormal, V, ao, coatTakesReflection ? reflection : float4(0.0));
        color = color * (1.0 - coat.factor * coatFresnel) + coat.factor * (coatAmbient + coatAccum);
    }
    if (pathTracedIndirect)
    {
        // The lobes carry the sheen's and the coat's attenuation of the base already.
        PathTraceLobes lobes = PathTraceLobesOf(N, V, albedo, metallic, roughness, specular, coat, sheen);
        float3 diffuse = pathTracedDiffuse * lobes.diffuse;
        if (Transmits(specular))
        {
            // A forward-shaded transmissive surface: the light from behind it takes the share of the
            // diffuse its transmission says, as in EvaluateSkyAmbient (the back's irradiance for
            // diffuse transmission from the probes, which the paths do not trace).
            float3 dielectric = EvaluateBaseSpecularAlbedos(SampleEnvironmentBrdf(roughness, max(dot(N, V), 0.0)), albedo, metallic, specular).dielectric;
            float3 front = pathTracedDiffuse * albedo * (1.0 - metallic) * (float3(1.0) - dielectric);
            front = MixDiffuseTransmittedAmbient(front, SceneDiffuseAmbient(worldPosition, -N, V) * ao, metallic, dielectric, specular);
            diffuse = MixTransmittedAmbient(front, metallic, dielectric, specular) * lobes.baseScale;
        }
        color += diffuse + pathTracedSpecular * (lobes.specular + lobes.coat);
    }
    return color;
}

// ---------------------------------------------------------------------------
// Volume scatter (KHR_materials_volume_scatter)
// ---------------------------------------------------------------------------

// One light's diffuse light entering a scattering surface, as the Khronos sample viewer's scatter
// pre-pass gathers it (scatter.frag): from in front, the Lambertian lobe through the transmission
// colour and the single-scatter albedo; from behind, the light through the volume
// (backAttenuation) that is not absorbed on the way, (1 - singleScatter) of it, scattered again. The
// dielectric's Fresnel weights both, about the light mirrored through the surface from behind.
float3 ScatterEnteringDirect(float3 N, float3 V, float3 L, float3 radiance, float3 color, float3 backAttenuation, float3 singleScatter, SpecularParams specular)
{
    float NdL = dot(N, L);
    float3 lightDirection = NdL < 0.0 ? normalize(L - 2.0 * N * NdL) : L;
    float3 F = FresnelSchlick(max(dot(V, normalize(lightDirection + V)), 0.0), specular.dielectricF0, specular.dielectricF90);
    float3 entering = NdL > 0.0 ? color * singleScatter * NdL : color * backAttenuation * (1.0 - singleScatter) * singleScatter * -NdL;
    return (float3(1.0) - F) * entering * radiance / PI;
}

// The scatter pre-pass's colour for one surface point (in radiance; the caller pre-exposes it): the
// image-based light from both sides and every directional and local light (shadowed, unlike the
// viewer, which has no shadows; area lights are left out, as the viewer has none), all through the
// diffuse transmission factor, the base's Fresnel and the sheen's scaling. No ambient occlusion, as
// in the viewer. color is the diffuse transmission colour (factor times map), backAttenuation the
// volume's attenuation through the thickness.
float3 ScatterEnteringLight(
    float3 worldPosition, float3 N, float3 geoNormal, float3 V, float roughness,
    float diffuseTransmissionFactor, float3 color, float3 backAttenuation, float3 singleScatter,
    SheenParams sheen, SpecularParams specular)
{
    float NdV = max(dot(N, V), 0.0);
    float2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    float3 specularAlbedo = BaseSpecularAlbedo(environmentBrdf, specular.dielectricF0, specular.dielectricF90, specular) *
                          SpecularEnergyCompensation(specular.dielectricF0, environmentBrdf);
    float3 front;
    float3 back;
    front = SceneDiffuseAmbient(worldPosition, N, V);
    back = SceneDiffuseAmbient(worldPosition, -N, V);
    float3 light = (float3(1.0) - specularAlbedo) * color * singleScatter * (front + back * backAttenuation * (1.0 - singleScatter));

    int shadowLightIndex = int(ubo.shadowParams.x);
    for (uint i = 0u; i < ubo.lightCounts.x; ++i)
    {
        float3 L;
        float3 radiance;
        LightSource source;
        if (!PunctualIncidence(sceneLights[i], worldPosition, L, radiance, source))
        {
            continue;
        }
        float3 contribution = ScatterEnteringDirect(N, V, L, radiance, color, backAttenuation, singleScatter, specular);
        if (int(i) == shadowLightIndex && any(contribution > float3(0.0)))
        {
            contribution *= EvaluateDirectionalShadow(worldPosition, ShadowOffsetNormal(geoNormal, L, true));
        }
        light += contribution;
    }
    uint localBegin = ubo.lightCounts.x;
    uint localCount = ubo.lightCounts.y - ubo.lightCounts.x;
    uint2 range = uint2(0u, localCount);
    if (ubo.lightCounts.z != 0u)
    {
        range = LightClusterRange(FindLightCluster(worldPosition));
    }
    for (uint k = 0u; k < range.y; ++k)
    {
        SceneLightData sceneLight = ubo.lightCounts.z != 0u ? sceneLights[LightClusterIndex(range.x + k)] : sceneLights[localBegin + k];
        float3 L;
        float3 radiance;
        LightSource source;
        if (!PunctualIncidence(sceneLight, worldPosition, L, radiance, source))
        {
            continue;
        }
        float3 contribution = ScatterEnteringDirect(N, V, L, radiance, color, backAttenuation, singleScatter, specular);
        float3 noCoat = float3(0.0);
        float3 noSheen = float3(0.0);
        ApplyLocalShadow(sceneLight, worldPosition, geoNormal, true, contribution, noCoat, noSheen);
        light += contribution;
    }

    if (HasSheen(sheen))
    {
        light *= 1.0 - max(sheen.color.r, max(sheen.color.g, sheen.color.b)) * SampleSheenAlbedo(sheen.roughness, NdV);
    }
    return light * diffuseTransmissionFactor;
}

#endif
