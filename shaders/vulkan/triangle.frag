#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 0) const bool kAlphaMask = false;
// The scatter pre-pass (VulkanScatterPass): writes the light entering a scattering surface and its
// draw slot instead of the shaded colour.
layout(constant_id = 1) const bool kScatterPrepass = false;

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "normal_map.glsl"
#include "material_common.glsl"
#include "material_uv.glsl"
// SHADING_MODEL_* for the material's shading model id.
#include "gbuffer_common.glsl"
#include "specular_aa.glsl"
#include "pre_exposure.glsl"

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicTexture;
layout(set = 1, binding = 3) uniform sampler2D roughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 6) uniform sampler2D secondaryBaseColorTexture;
layout(set = 1, binding = 7) uniform sampler2D secondaryNormalTexture;
layout(set = 1, binding = 8) uniform sampler2D secondaryMetallicTexture;
layout(set = 1, binding = 9) uniform sampler2D secondaryRoughnessTexture;
layout(set = 1, binding = 10) uniform sampler2D secondaryOcclusionTexture;
layout(set = 1, binding = 11) uniform sampler2D secondaryEmissiveTexture;
layout(set = 1, binding = 12) uniform sampler2D blendMaskTexture;
#include "material_layers.glsl"
#include "transmission_common.glsl"
#include "volume_scatter_common.glsl"

// The scene behind transmissive surfaces (VulkanTransmissionCopyPass): pre-exposed, full mip chain.
layout(set = 0, binding = 18) uniform sampler2D transmissionCopy;
// The scatter pre-pass (VulkanScatterPass): the light entering each scattering surface, pre-exposed,
// with its draw slot + 1 in alpha (0 where nothing scatters), and that surface's own depth.
layout(set = 0, binding = 19) uniform sampler2D scatterLight;
layout(set = 0, binding = 20) uniform sampler2D scatterDepth;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in vec4 fragWorldTangent;
layout(location = 4) in vec3 fragWorldPosition;
layout(location = 7) flat in uint fragDrawSlot;
layout(location = 8) in vec2 fragTexCoord1;
layout(location = 9) flat in vec3 fragModelScale;

layout(location = 0) out vec4 outColor;

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
// What is behind a transmissive surface for one IOR: the transmission copy where the view ray
// refracted by it leaves the volume (straight behind a thin wall), at the viewer's blur for the
// material's roughness, in radiance (the copy is pre-exposed); a = the ray's length in the volume.
vec4 SampleTransmission(vec3 N, vec3 V, float ior, float thickness, vec3 volumeScale, float materialRoughness)
{
    vec3 exitPoint = TransmissionExitPoint(fragWorldPosition, N, V, ior, thickness, volumeScale);
    vec4 exitClip = ubo.proj * ubo.view * vec4(exitPoint, 1.0);
    vec2 exitUv = exitClip.xy / exitClip.w * 0.5 + 0.5;
    vec3 behind = textureLod(transmissionCopy, exitUv, TransmissionLod(materialRoughness, ior)).rgb / max(ubo.exposure.x, 1e-20);
    return vec4(behind, length(exitPoint - fragWorldPosition));
}

// The pre-pass's id for a draw: its slot + 1, kept exact in the RGBA16F target's alpha (a half float
// holds every integer to 2048). Draws 2047 slots apart share an id, which only matters where two of
// them scatter side by side.
float ScatterDrawId(uint drawSlot)
{
    return float(drawSlot % 2047u + 1u);
}

// A pre-pass pixel's world position, from its own depth.
vec3 ScatterWorldPosition(vec2 uv, float depth)
{
    vec4 world = ubo.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

// KHR_materials_volume_scatter's diffusion, as the Khronos sample viewer gathers it
// (getSubsurfaceScattering): the pre-pass's light around this pixel, over a disk as wide as the
// largest channel of the scatter radius (attenuation distance times the multi-scatter colour), each
// sample of the same draw weighted by the Burley profile at its distance from this point over its
// pdf. In radiance. A disk within one pixel is the pixel itself. Unlike the viewer, whose disk is
// the width's texel size on both axes, the disk is round in pixels.
vec3 GatherSubsurfaceScattering(vec3 multiscatter, float attenuationDistance)
{
    vec2 extent = vec2(textureSize(scatterLight, 0));
    vec2 uv = gl_FragCoord.xy / extent;
    vec4 center = textureLod(scatterLight, uv, 0.0);
    float inverseExposure = 1.0 / max(ubo.exposure.x, 1e-20);
    vec3 scatterDistance = attenuationDistance * multiscatter;
    float maxDistance = max(scatterDistance.r, max(scatterDistance.g, scatterDistance.b));
    vec3 centerPosition = ScatterWorldPosition(uv, textureLod(scatterDepth, uv, 0.0).r);
    float metresPerPixel = distance(centerPosition, ScatterWorldPosition(uv + vec2(1.0 / extent.x, 0.0), textureLod(scatterDepth, uv, 0.0).r));
    float maxRadiusPixels = maxDistance / max(metresPerPixel, 1e-12);
    if (maxRadiusPixels <= 1.0)
    {
        return center.rgb * inverseExposure;
    }

    vec3 d = BurleyShape(max(vec3(BurleyMinimumRadius()), scatterDistance / maxDistance) * maxDistance);
    vec3 totalWeight = vec3(0.0);
    vec3 total = vec3(0.0);
    for (int i = 0; i < kScatterSampleCount; ++i)
    {
        vec3 scatterSample = BurleyScatterSample(i);
        vec2 offset = vec2(cos(scatterSample.x), sin(scatterSample.x)) * scatterSample.y * maxRadiusPixels / extent;
        vec2 sampleUv = uv + offset;
        vec4 sampled = textureLod(scatterLight, sampleUv, 0.0);
        if (sampled.a != center.a)
        {
            continue;
        }
        vec3 samplePosition = ScatterWorldPosition(sampleUv, textureLod(scatterDepth, sampleUv, 0.0).r);
        vec3 weight = BurleyProfile(d, distance(samplePosition, centerPosition)) * scatterSample.z;
        totalWeight += weight;
        total += weight * sampled.rgb;
    }
    return total / max(totalWeight, vec3(0.0001)) * inverseExposure;
}

void main()
{
    MaterialData material = materialData.materials[fragDrawSlot];

    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, material.nodeGraphFactors.y, clamp(material.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    vec4 primaryBaseColor = texture(baseColorTexture, MaterialSlotUv(material, fragDrawSlot, 0u, fragTexCoord, fragTexCoord1));
    vec4 secondaryBaseColor = texture(secondaryBaseColorTexture, fragTexCoord);
    vec4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * material.baseColorFactor;

    if (kAlphaMask && albedo.a < material.alphaCutoff)
        discard;

    // Unlit: the base colour as if lit to the display's paper white, at every exposure.
    if ((material.shadingModel.x & SHADING_FLAG_UNLIT) != 0u)
    {
        outColor = vec4(albedo.rgb * kFrameBufferUnitsPerExposed, albedo.a);
        return;
    }

    // ---- Normal -----------------------------------------------------------
    // A back face is only rasterized by a double-sided pipeline, and it is seen from the side the
    // vertex normal points away from. Mirroring the whole tangent frame, bitangent included, shades
    // it as the front face would be from the other side, with the normal map's perturbation
    // mirrored along with it. Single-sided pipelines cull back faces, so this is a no-op for them.
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 geoNormal = normalize(fragWorldNormal) * faceSign;
    vec3 faceTangent = fragWorldTangent.xyz * faceSign;
    vec3 tangent = normalize(faceTangent - geoNormal * dot(geoNormal, faceTangent));
    // cross(-N, -T) == cross(N, T), so the flip has to be applied to the bitangent explicitly.
    vec3 bitangent = normalize(cross(geoNormal, tangent) * fragWorldTangent.w) * faceSign;
    mat3 TBN = mat3(tangent, bitangent, geoNormal);

    vec3 nrmPrimary = DecodeNormalMap(texture(normalTexture, MaterialSlotUv(material, fragDrawSlot, 1u, fragTexCoord, fragTexCoord1)));
    nrmPrimary.xy = RotateMaterialTangentXy(material, fragDrawSlot, 1u, nrmPrimary.xy);
    vec3 nrmSecondary = DecodeNormalMap(texture(secondaryNormalTexture, fragTexCoord));
    vec3 nrmSample = normalize(mix(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= material.surfaceFactors.z; // normal scale
    vec3 N = normalize(TBN * nrmSample);

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = mix(
        texture(metallicTexture, MaterialSlotUv(material, fragDrawSlot, 2u, fragTexCoord, fragTexCoord1)).b,
        texture(secondaryMetallicTexture, fragTexCoord).b,
        blendWeight);
    float roughnessSample = mix(
        texture(roughnessTexture, MaterialSlotUv(material, fragDrawSlot, 3u, fragTexCoord, fragTexCoord1)).g,
        texture(secondaryRoughnessTexture, fragTexCoord).g,
        blendWeight);
    float aoSample = mix(
        texture(occlusionTexture, MaterialSlotUv(material, fragDrawSlot, 4u, fragTexCoord, fragTexCoord1)).r,
        texture(secondaryOcclusionTexture, fragTexCoord).r,
        blendWeight);
    vec3 emissiveSample = mix(
        texture(emissiveTexture, MaterialSlotUv(material, fragDrawSlot, 5u, fragTexCoord, fragTexCoord1)).rgb,
        texture(secondaryEmissiveTexture, fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(material.surfaceFactors.x * metallicSample, 0.0, 1.0);
    // The material's own roughness, before the floor and specular AA below: what blurs transmitted
    // light (the transmission copy's LOD, as the Khronos sample viewer takes it). Geometric specular
    // AA widens reflections against aliasing; it would fog clear glass.
    float materialRoughness = clamp(material.surfaceFactors.y * roughnessSample, 0.0, 1.0);
    float roughness = clamp(material.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    // As gbuffer.frag: both variations here, in uniform control flow.
    roughness = FilterRoughnessForSpecularAA(roughness, NormalVariation(N));
    MaterialLayers layers = EvaluateMaterialLayers(material, fragDrawSlot, fragTexCoord, fragTexCoord1, TBN, N);
    float coatNormalVariation = NormalVariation(layers.coatNormal);
    float ao = mix(1.0, aoSample, clamp(material.surfaceFactors.w, 0.0, 1.0));

    vec3 V = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);

    // ---- Shade ------------------------------------------------------------
    // ShadeSurface is the ambient, direct and shadow arithmetic deferred_lighting.frag also runs;
    // it lives in pbr_common.glsl so the forward comparison path and the deferred path cannot
    // drift.
    vec3 emissive = emissiveSample * material.emissiveFactor;
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
            material.transmissionFactors.x * texture(transmissionTexture, MaterialSlotUv(material, fragDrawSlot, 23u, fragTexCoord, fragTexCoord1)).r,
            0.0, 1.0);
        float thickness = material.transmissionFactors.y * texture(thicknessTexture, MaterialSlotUv(material, fragDrawSlot, 24u, fragTexCoord, fragTexCoord1)).g;
        float ior = max(material.attenuationColor.a, 1.0);
        vec3 volumeScale = fragModelScale * material.volumeScale.xyz;
        vec3 behind;
        vec3 absorption;
        float dispersion = material.transmissionFactors.w;
        if (dispersion > 0.0 && thickness > 0.0)
        {
            // KHR_materials_dispersion: each channel refracts by its own IOR and takes its own
            // sample, blur and absorption along its own ray.
            vec3 iors = DispersedIors(ior, dispersion);
            for (int channel = 0; channel < 3; ++channel)
            {
                vec4 sampled = SampleTransmission(N, V, iors[channel], thickness, volumeScale, materialRoughness);
                behind[channel] = sampled[channel];
                absorption[channel] = ApplyVolumeAttenuation(vec3(1.0), sampled.a, material.attenuationColor.rgb, material.transmissionFactors.z)[channel];
            }
        }
        else
        {
            vec4 sampled = SampleTransmission(N, V, ior, thickness, volumeScale, materialRoughness);
            behind = sampled.rgb;
            absorption = ApplyVolumeAttenuation(vec3(1.0), sampled.a, material.attenuationColor.rgb, material.transmissionFactors.z);
        }
        specular.transmissionFactor = transmission;
        specular.transmittedRadiance = behind * absorption * albedo.rgb;
        specular.transmissionTint = absorption * albedo.rgb;
        specular.transmissionAlpha = max(TransmissionRoughness(roughness * roughness, ior), 1e-3);
    }
    vec3 diffuseTransmissionColor = vec3(0.0);
    vec3 diffuseTransmissionAttenuation = vec3(1.0);
    if (material.diffuseTransmission.a > 0.0)
    {
        // KHR_materials_diffuse_transmission: the factor times its map's A, the colour times its map,
        // attenuated through the volume's thickness (times the node's mean scale) when it has one.
        specular.diffuseTransmissionFactor = clamp(
            material.diffuseTransmission.a * texture(diffuseTransmissionTexture, MaterialSlotUv(material, fragDrawSlot, 25u, fragTexCoord, fragTexCoord1)).a,
            0.0, 1.0);
        diffuseTransmissionColor = material.diffuseTransmission.rgb *
                                   texture(diffuseTransmissionColorTexture, MaterialSlotUv(material, fragDrawSlot, 26u, fragTexCoord, fragTexCoord1)).rgb;
        float thickness = material.transmissionFactors.y * texture(thicknessTexture, MaterialSlotUv(material, fragDrawSlot, 24u, fragTexCoord, fragTexCoord1)).g;
        float distance = DiffuseTransmissionDistance(thickness, fragModelScale * material.volumeScale.xyz);
        diffuseTransmissionAttenuation = ApplyVolumeAttenuation(vec3(1.0), distance, material.attenuationColor.rgb, material.transmissionFactors.z);
        specular.diffuseTransmissionColor = diffuseTransmissionColor * diffuseTransmissionAttenuation;
    }
    // KHR_materials_volume_scatter: of the light through the volume, the single-scatter albedo's share
    // scatters (the pre-pass gathers it, the gather below diffuses it) and the rest passes.
    bool scatters = material.volumeScale.w > 0.5;
    vec3 singleScatter = scatters ? MultiToSingleScatter(material.volumeScatter.rgb) : vec3(0.0);
    if (kScatterPrepass)
    {
        vec3 entering = scatters ? ScatterEnteringLight(
                                       fragWorldPosition, N, geoNormal, V, roughness, specular.diffuseTransmissionFactor,
                                       diffuseTransmissionColor, diffuseTransmissionAttenuation, singleScatter, sheen, specular)
                                 : vec3(0.0);
        outColor = vec4(entering * ubo.exposure.x, ScatterDrawId(fragDrawSlot));
        return;
    }
    specular.diffuseTransmissionColor *= vec3(1.0) - singleScatter;
    // The forward path has no screen-space reflection: the environment alone, specularly occluded.
    vec3 color = ShadeSurface(fragWorldPosition, N, geoNormal, V, albedo.rgb, metallic, roughness, ao, emissive, coat, sheen, anisotropy, specular, vec4(0.0));
    if (scatters)
    {
        // The diffused light leaves through the transmission colour, weighted as the viewer weights it:
        // not through metal, the coat's reflection, a film or specular transmission.
        float coatFresnel = coat.factor > 0.0 ? FresnelSchlick(max(dot(coat.normal, V), 0.0), COAT_F0).x : 0.0;
        color += GatherSubsurfaceScattering(material.volumeScatter.rgb, material.transmissionFactors.z) * diffuseTransmissionColor *
                 (1.0 - metallic) * (1.0 - coat.factor * coatFresnel) * (1.0 - specular.iridescenceFactor) * (1.0 - specular.transmissionFactor);
    }

    // The atmosphere between the surface and the camera, before blending: an approximation for
    // Blend items, exact for the forward-only order's opaque ones.
    color = ApplyAerialPerspective(color, fragWorldPosition) * ubo.exposure.x;

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance, pre-exposed (see pre_exposure.glsl).
    outColor = vec4(color, albedo.a);
}
