#include "specialization.slang"

SPECIALIZATION_BOOL(0, kAlphaMask);
// The scatter pre-pass (VulkanScatterPass): writes the light entering a scattering surface and its
// draw slot instead of the shaded colour.
SPECIALIZATION_BOOL(1, kScatterPrepass);

#include "shader_helpers.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "pbr_common.slang"
#include "normal_map.slang"
#include "material_common.slang"
#include "material_uv.slang"
// SHADING_MODEL_* for the material's shading model id.
#include "gbuffer_common.slang"
#include "specular_aa.slang"
#include "pre_exposure.slang"

[[vk::binding(0, 1)]] Texture2D baseColorTexture : register(t0, space1);
#define baseColorTextureSampler MaterialSampler(0)
[[vk::binding(1, 1)]] Texture2D normalTexture : register(t1, space1);
#define normalTextureSampler MaterialSampler(1)
[[vk::binding(2, 1)]] Texture2D metallicTexture : register(t2, space1);
#define metallicTextureSampler MaterialSampler(2)
[[vk::binding(3, 1)]] Texture2D roughnessTexture : register(t3, space1);
#define roughnessTextureSampler MaterialSampler(3)
[[vk::binding(4, 1)]] Texture2D occlusionTexture : register(t4, space1);
#define occlusionTextureSampler MaterialSampler(4)
[[vk::binding(5, 1)]] Texture2D emissiveTexture : register(t5, space1);
#define emissiveTextureSampler MaterialSampler(5)
[[vk::binding(6, 1)]] Texture2D secondaryBaseColorTexture : register(t6, space1);
#define secondaryBaseColorTextureSampler MaterialSampler(6)
[[vk::binding(7, 1)]] Texture2D secondaryNormalTexture : register(t7, space1);
#define secondaryNormalTextureSampler MaterialSampler(7)
[[vk::binding(8, 1)]] Texture2D secondaryMetallicTexture : register(t8, space1);
#define secondaryMetallicTextureSampler MaterialSampler(8)
[[vk::binding(9, 1)]] Texture2D secondaryRoughnessTexture : register(t9, space1);
#define secondaryRoughnessTextureSampler MaterialSampler(9)
[[vk::binding(10, 1)]] Texture2D secondaryOcclusionTexture : register(t10, space1);
#define secondaryOcclusionTextureSampler MaterialSampler(10)
[[vk::binding(11, 1)]] Texture2D secondaryEmissiveTexture : register(t11, space1);
#define secondaryEmissiveTextureSampler MaterialSampler(11)
[[vk::binding(12, 1)]] Texture2D blendMaskTexture : register(t12, space1);
#define blendMaskTextureSampler MaterialSampler(12)
#include "material_layers.slang"
#include "detail_layers.slang"
#include "transmission_common.slang"
#include "volume_scatter_common.slang"

// The scene behind transmissive surfaces (VulkanTransmissionCopyPass): pre-exposed, full mip chain.
[[vk::binding(18, 0)]] Texture2D transmissionCopy : register(t18, space0);
[[vk::binding(FRAME_SAMPLER(18), 0)]] SamplerState transmissionCopySampler : register(s82, space0);
// The scatter pre-pass (VulkanScatterPass): the light entering each scattering surface, pre-exposed,
// with its draw slot + 1 in alpha (0 where nothing scatters), and that surface's own depth.
[[vk::binding(19, 0)]] Texture2D scatterLight : register(t19, space0);
[[vk::binding(FRAME_SAMPLER(19), 0)]] SamplerState scatterLightSampler : register(s83, space0);
[[vk::binding(20, 0)]] Texture2D scatterDepth : register(t20, space0);
[[vk::binding(FRAME_SAMPLER(20), 0)]] SamplerState scatterDepthSampler : register(s84, space0);
// Path tracing mode (camera block textureParams.y): the nearest forward-shaded surface's depth
// (VulkanPathTraceLayerPass) and the light the path tracer found for it through its diffuse and
// specular lobes, demodulated and pre-exposed (VulkanPathTracePass).
[[vk::binding(29, 0)]] Texture2D pathTraceLayerDepth : register(t29, space0);
[[vk::binding(30, 0)]] Texture2D pathTraceLayerDiffuse : register(t30, space0);
[[vk::binding(31, 0)]] Texture2D pathTraceLayerSpecular : register(t31, space0);

struct FragmentInput
{
    [[vk::location(0)]] float3 fragColor : TEXCOORD0;
    [[vk::location(1)]] float2 fragTexCoord : TEXCOORD1;
    [[vk::location(2)]] float3 fragWorldNormal : TEXCOORD2;
    [[vk::location(3)]] float4 fragWorldTangent : TEXCOORD3;
    [[vk::location(4)]] float3 fragWorldPosition : TEXCOORD4;
    [[vk::location(7)]] nointerpolation uint fragDrawSlot : TEXCOORD7;
    [[vk::location(8)]] float2 fragTexCoord1 : TEXCOORD8;
    [[vk::location(10)]] float3 fragObjectPosition : TEXCOORD10;
    [[vk::location(9)]] nointerpolation float3 fragModelScale : TEXCOORD9;
    float4 fragCoord : SV_Position;
    bool frontFacing : SV_IsFrontFace;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
// What is behind a transmissive surface for one IOR: the transmission copy where the view ray
// refracted by it leaves the volume (straight behind a thin wall), at the viewer's blur for the
// material's roughness, in radiance (the copy is pre-exposed); a = the ray's length in the volume.
float4 SampleTransmission(float3 worldPosition, float3 N, float3 V, float ior, float thickness, float3 volumeScale, float materialRoughness)
{
    float3 exitPoint = TransmissionExitPoint(worldPosition, N, V, ior, thickness, volumeScale);
    float4 exitClip = mul(mul(ubo.proj, ubo.view), float4(exitPoint, 1.0));
    float2 exitUv = exitClip.xy / exitClip.w * 0.5 + 0.5;
    float3 behind = transmissionCopy.SampleLevel(transmissionCopySampler, exitUv, TransmissionLod(materialRoughness, ior)).rgb / max(ubo.exposure.x, 1e-20);
    return float4(behind, length(exitPoint - worldPosition));
}

// The pre-pass's id for a draw: its slot + 1, kept exact in the RGBA16F target's alpha (a half float
// holds every integer to 2048). Draws 2047 slots apart share an id, which only matters where two of
// them scatter side by side.
float ScatterDrawId(uint drawSlot)
{
    return float(drawSlot % 2047u + 1u);
}

// A pre-pass pixel's world position, from its own depth.
float3 ScatterWorldPosition(float2 uv, float depth)
{
    float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return world.xyz / world.w;
}

// KHR_materials_volume_scatter's diffusion, as the Khronos sample viewer gathers it
// (getSubsurfaceScattering): the pre-pass's light around this pixel, over a disk as wide as the
// largest channel of the scatter radius (attenuation distance times the multi-scatter colour), each
// sample of the same draw weighted by the Burley profile at its distance from this point over its
// pdf. In radiance. A disk within one pixel is the pixel itself. Unlike the viewer, whose disk is
// the width's texel size on both axes, the disk is round in pixels.
float3 GatherSubsurfaceScattering(float2 pixel, float3 multiscatter, float attenuationDistance)
{
    float2 extent = float2(TextureSize(scatterLight, 0));
    float2 uv = pixel / extent;
    float4 center = scatterLight.SampleLevel(scatterLightSampler, uv, 0.0);
    float inverseExposure = 1.0 / max(ubo.exposure.x, 1e-20);
    float3 scatterDistance = attenuationDistance * multiscatter;
    float maxDistance = max(scatterDistance.r, max(scatterDistance.g, scatterDistance.b));
    float3 centerPosition = ScatterWorldPosition(uv, scatterDepth.SampleLevel(scatterDepthSampler, uv, 0.0).r);
    float metresPerPixel = distance(centerPosition, ScatterWorldPosition(uv + float2(1.0 / extent.x, 0.0), scatterDepth.SampleLevel(scatterDepthSampler, uv, 0.0).r));
    float maxRadiusPixels = maxDistance / max(metresPerPixel, 1e-12);
    if (maxRadiusPixels <= 1.0)
    {
        return center.rgb * inverseExposure;
    }

    float3 d = BurleyShape(max(float3(BurleyMinimumRadius()), scatterDistance / maxDistance) * maxDistance);
    float3 totalWeight = float3(0.0);
    float3 total = float3(0.0);
    for (int i = 0; i < kScatterSampleCount; ++i)
    {
        float3 scatterSample = BurleyScatterSample(i);
        float2 offset = float2(cos(scatterSample.x), sin(scatterSample.x)) * scatterSample.y * maxRadiusPixels / extent;
        float2 sampleUv = uv + offset;
        float4 sampled = scatterLight.SampleLevel(scatterLightSampler, sampleUv, 0.0);
        if (sampled.a != center.a)
        {
            continue;
        }
        float3 samplePosition = ScatterWorldPosition(sampleUv, scatterDepth.SampleLevel(scatterDepthSampler, sampleUv, 0.0).r);
        float3 weight = BurleyProfile(d, distance(samplePosition, centerPosition)) * scatterSample.z;
        totalWeight += weight;
        total += weight * sampled.rgb;
    }
    return total / max(totalWeight, float3(0.0001)) * inverseExposure;
}

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    MaterialData material = materials[input.fragDrawSlot];
    LoadMaterialSamplers(material.samplerIndices);

    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = MaterialTexture(blendMaskTexture, input.fragTexCoord).r;
    float blendWeight = clamp(
        lerp(0.0, material.nodeGraphFactors.y, clamp(material.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    float4 primaryBaseColor = MaterialTexture(baseColorTexture, MaterialSlotUv(material, input.fragDrawSlot, 0u, input.fragTexCoord, input.fragTexCoord1));
    float4 secondaryBaseColor = MaterialTexture(secondaryBaseColorTexture, input.fragTexCoord);
    float4 sampledBaseColor = lerp(primaryBaseColor, secondaryBaseColor, blendWeight);
    float4 albedo = sampledBaseColor * float4(input.fragColor, 1.0) * material.baseColorFactor;
    albedo.rgb *= DetailLayersFactor(material, input.fragTexCoord, input.fragObjectPosition);

    if (kAlphaMask && albedo.a < material.alphaCutoff)
        discard;

    // Unlit: the base colour as if lit to the display's paper white, at every exposure.
    if ((material.shadingModel.x & SHADING_FLAG_UNLIT) != 0u)
    {
        output.outColor = float4(albedo.rgb * kFrameBufferUnitsPerExposed, albedo.a);
        return output;
    }

    // ---- Normal -----------------------------------------------------------
    // A back face is only rasterized by a double-sided pipeline, and it is seen from the side the
    // vertex normal points away from. Mirroring the whole tangent frame, bitangent included, shades
    // it as the front face would be from the other side, with the normal map's perturbation
    // mirrored along with it. Single-sided pipelines cull back faces, so this is a no-op for them.
    float faceSign = input.frontFacing ? 1.0 : -1.0;
    float3 geoNormal = normalize(input.fragWorldNormal) * faceSign;
    float3 faceTangent = input.fragWorldTangent.xyz * faceSign;
    float3 tangent = normalize(faceTangent - geoNormal * dot(geoNormal, faceTangent));
    // cross(-N, -T) == cross(N, T), so the flip has to be applied to the bitangent explicitly.
    float3 bitangent = normalize(cross(geoNormal, tangent) * input.fragWorldTangent.w) * faceSign;
    float3x3 TBN = MatrixFromColumns(tangent, bitangent, geoNormal);

    float3 nrmPrimary = DecodeNormalMap(MaterialTexture(normalTexture, MaterialSlotUv(material, input.fragDrawSlot, 1u, input.fragTexCoord, input.fragTexCoord1)));
    nrmPrimary.xy = RotateMaterialTangentXy(material, input.fragDrawSlot, 1u, nrmPrimary.xy);
    float3 nrmSecondary = DecodeNormalMap(MaterialTexture(secondaryNormalTexture, input.fragTexCoord));
    float3 nrmSample = normalize(lerp(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= material.surfaceFactors.z; // normal scale
    float3 N = normalize(mul(TBN, nrmSample));

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = lerp(
        MaterialTexture(metallicTexture, MaterialSlotUv(material, input.fragDrawSlot, 2u, input.fragTexCoord, input.fragTexCoord1)).b,
        MaterialTexture(secondaryMetallicTexture, input.fragTexCoord).b,
        blendWeight);
    float roughnessSample = lerp(
        MaterialTexture(roughnessTexture, MaterialSlotUv(material, input.fragDrawSlot, 3u, input.fragTexCoord, input.fragTexCoord1)).g,
        MaterialTexture(secondaryRoughnessTexture, input.fragTexCoord).g,
        blendWeight);
    float aoSample = lerp(
        MaterialTexture(occlusionTexture, MaterialSlotUv(material, input.fragDrawSlot, 4u, input.fragTexCoord, input.fragTexCoord1)).r,
        MaterialTexture(secondaryOcclusionTexture, input.fragTexCoord).r,
        blendWeight);
    float3 emissiveSample = lerp(
        MaterialTexture(emissiveTexture, MaterialSlotUv(material, input.fragDrawSlot, 5u, input.fragTexCoord, input.fragTexCoord1)).rgb,
        MaterialTexture(secondaryEmissiveTexture, input.fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(material.surfaceFactors.x * metallicSample, 0.0, 1.0);
    // The material's own roughness, before the floor and specular AA below: what blurs transmitted
    // light (the transmission copy's LOD, as the Khronos sample viewer takes it). Geometric specular
    // AA widens reflections against aliasing; it would fog clear glass.
    float materialRoughness = clamp(material.surfaceFactors.y * roughnessSample, 0.0, 1.0);
    float roughness = clamp(material.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    // As gbuffer.frag: both variations here, in uniform control flow.
    roughness = FilterRoughnessForSpecularAA(roughness, NormalVariation(N));
    MaterialLayers layers = EvaluateMaterialLayers(material, input.fragDrawSlot, input.fragTexCoord, input.fragTexCoord1, TBN, N);
    float coatNormalVariation = NormalVariation(layers.coatNormal);
    float ao = lerp(1.0, aoSample, clamp(material.surfaceFactors.w, 0.0, 1.0));

    float3 V = normalize(ubo.cameraWorldPosition.xyz - input.fragWorldPosition);

    // ---- Shade ------------------------------------------------------------
    // ShadeSurface is the ambient, direct and shadow arithmetic deferred_lighting.frag also runs;
    // it lives in pbr_common.slang so the forward comparison path and the deferred path cannot
    // drift.
    float3 emissive = emissiveSample * material.emissiveFactor;
    // The forward path reads the layers from the material and its maps, where the deferred path
    // reads them from GB5.
    CoatParams coat = NoCoat();
    SheenParams sheen = NoSheen();
    AnisotropyParams anisotropy = NoAnisotropy();
    SpecularParams specular = NoSpecularOverride();
    if (HasShadingFlag(layers.flags, SHADING_FLAG_CLEARCOAT))
    {
        coat.factor = layers.coatFactor;
        coat.roughness = FilterRoughnessForSpecularAA(clamp(layers.coatRoughness, 0.04, 1.0), coatNormalVariation);
        coat.normal = layers.coatNormal;
    }
    if (HasShadingFlag(layers.flags, SHADING_FLAG_SHEEN))
    {
        sheen.color = layers.sheenColor;
        sheen.roughness = clamp(layers.sheenRoughness, 0.04, 1.0);
    }
    if (HasShadingFlag(layers.flags, SHADING_FLAG_ANISOTROPY))
    {
        anisotropy.tangent = layers.anisotropyTangent;
        anisotropy.strength = layers.anisotropyStrength;
    }
    specular.dielectricF0 = layers.dielectricF0;
    specular.dielectricF90 = layers.dielectricF90;
    if (layers.iridescenceFactor > 0.0)
    {
        // The film's Fresnel once per pixel at N.V, as the Khronos sample viewer evaluates it, over
        // the surface's own F0.
        specular.iridescenceFactor = layers.iridescenceFactor;
        specular.iridescenceFresnel = EvaluateIridescence(
            material.iridescenceFactors.y, max(dot(N, V), 1e-4), layers.iridescenceThickness,
            SurfaceF0(albedo.rgb, metallic, specular));
    }
    if (HasShadingFlag(material.shadingModel.x, SHADING_FLAG_TRANSMISSION))
    {
        // What is behind the surface, where the view ray leaves the volume (straight behind a thin
        // wall), from the transmission copy at the viewer's blur for the material's roughness. The copy holds
        // pre-exposed values; the shading below is in radiance until its final exposure.
        float transmission = clamp(
            material.transmissionFactors.x * MaterialTexture(transmissionTexture, MaterialSlotUv(material, input.fragDrawSlot, 23u, input.fragTexCoord, input.fragTexCoord1)).r,
            0.0, 1.0);
        float thickness = material.transmissionFactors.y * MaterialTexture(thicknessTexture, MaterialSlotUv(material, input.fragDrawSlot, 24u, input.fragTexCoord, input.fragTexCoord1)).g;
        float ior = max(material.attenuationColor.a, 1.0);
        float3 volumeScale = input.fragModelScale * material.volumeScale.xyz;
        float3 behind;
        float3 absorption;
        float dispersion = material.transmissionFactors.w;
        if (dispersion > 0.0 && thickness > 0.0)
        {
            // KHR_materials_dispersion: each channel refracts by its own IOR and takes its own
            // sample, blur and absorption along its own ray.
            float3 iors = DispersedIors(ior, dispersion);
            for (int channel = 0; channel < 3; ++channel)
            {
                float4 sampled = SampleTransmission(input.fragWorldPosition, N, V, iors[channel], thickness, volumeScale, materialRoughness);
                behind[channel] = sampled[channel];
                absorption[channel] = ApplyVolumeAttenuation(float3(1.0), sampled.a, material.attenuationColor.rgb, material.transmissionFactors.z)[channel];
            }
        }
        else
        {
            float4 sampled = SampleTransmission(input.fragWorldPosition, N, V, ior, thickness, volumeScale, materialRoughness);
            behind = sampled.rgb;
            absorption = ApplyVolumeAttenuation(float3(1.0), sampled.a, material.attenuationColor.rgb, material.transmissionFactors.z);
        }
        specular.transmissionFactor = transmission;
        specular.transmittedRadiance = behind * absorption * albedo.rgb;
        specular.transmissionTint = absorption * albedo.rgb;
        specular.transmissionAlpha = max(TransmissionRoughness(roughness * roughness, ior), 1e-3);
    }
    float3 diffuseTransmissionColor = float3(0.0);
    float3 diffuseTransmissionAttenuation = float3(1.0);
    if (material.diffuseTransmission.a > 0.0)
    {
        // KHR_materials_diffuse_transmission: the factor times its map's A, the colour times its map,
        // attenuated through the volume's thickness (times the node's mean scale) when it has one.
        specular.diffuseTransmissionFactor = clamp(
            material.diffuseTransmission.a * MaterialTexture(diffuseTransmissionTexture, MaterialSlotUv(material, input.fragDrawSlot, 25u, input.fragTexCoord, input.fragTexCoord1)).a,
            0.0, 1.0);
        diffuseTransmissionColor = material.diffuseTransmission.rgb *
                                   MaterialTexture(diffuseTransmissionColorTexture, MaterialSlotUv(material, input.fragDrawSlot, 26u, input.fragTexCoord, input.fragTexCoord1)).rgb;
        float thickness = material.transmissionFactors.y * MaterialTexture(thicknessTexture, MaterialSlotUv(material, input.fragDrawSlot, 24u, input.fragTexCoord, input.fragTexCoord1)).g;
        float distance = DiffuseTransmissionDistance(thickness, input.fragModelScale * material.volumeScale.xyz);
        diffuseTransmissionAttenuation = ApplyVolumeAttenuation(float3(1.0), distance, material.attenuationColor.rgb, material.transmissionFactors.z);
        specular.diffuseTransmissionColor = diffuseTransmissionColor * diffuseTransmissionAttenuation;
    }
    // KHR_materials_volume_scatter: of the light through the volume, the single-scatter albedo's share
    // scatters (the pre-pass gathers it, the gather below diffuses it) and the rest passes.
    bool scatters = material.volumeScale.w > 0.5;
    float3 singleScatter = scatters ? MultiToSingleScatter(material.volumeScatter.rgb) : float3(0.0);
    if (kScatterPrepass)
    {
        float3 entering = scatters ? ScatterEnteringLight(
                                       input.fragWorldPosition, N, geoNormal, V, roughness, specular.diffuseTransmissionFactor,
                                       diffuseTransmissionColor, diffuseTransmissionAttenuation, singleScatter, sheen, specular)
                                 : float3(0.0);
        output.outColor = float4(entering * ubo.exposure.x, ScatterDrawId(input.fragDrawSlot));
        return output;
    }
    specular.diffuseTransmissionColor *= float3(1.0) - singleScatter;
    // In path tracing mode the nearest forward-shaded surface over each pixel was path traced: it
    // takes that light in place of the ambient terms, as the deferred surfaces do. Surfaces behind it
    // keep the environment and the probes.
    if (ubo.textureParams.y > 0.5)
    {
        int2 pixel = int2(input.fragCoord.xy);
        if (pathTraceLayerDepth.Load(int3(pixel, 0)).r == input.fragCoord.z)
        {
            pathTracedIndirect = true;
            // At half resolution (textureParams.z 1) each 2 x 2 block shares one traced pixel.
            int2 traced = pixel >> uint(ubo.textureParams.z);
            pathTracedDiffuse = pathTraceLayerDiffuse.Load(int3(traced, 0)).rgb * ubo.exposure.y;
            pathTracedSpecular = pathTraceLayerSpecular.Load(int3(traced, 0)).rgb * ubo.exposure.y;
        }
    }
    // The forward path has no screen-space reflection: the environment alone, specularly occluded.
    float3 color = ShadeSurface(input.fragWorldPosition, N, geoNormal, V, albedo.rgb, metallic, roughness, ao, emissive, coat, sheen, anisotropy, specular, float4(0.0));
    if (scatters)
    {
        // The diffused light leaves through the transmission colour, weighted as the viewer weights it:
        // not through metal, the coat's reflection, a film or specular transmission.
        float coatFresnel = coat.factor > 0.0 ? FresnelSchlick(max(dot(coat.normal, V), 0.0), COAT_F0).x : 0.0;
        color += GatherSubsurfaceScattering(input.fragCoord.xy, material.volumeScatter.rgb, material.transmissionFactors.z) * diffuseTransmissionColor *
                 (1.0 - metallic) * (1.0 - coat.factor * coatFresnel) * (1.0 - specular.iridescenceFactor) * (1.0 - specular.transmissionFactor);
    }

    // The atmosphere between the surface and the camera, before blending: an approximation for
    // Blend items, exact for the forward-only order's opaque ones.
    color = ApplyAerialPerspective(color, input.fragWorldPosition) * ubo.exposure.x;

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance, pre-exposed (see pre_exposure.slang).
    output.outColor = float4(color, albedo.a);
    return output;
}
