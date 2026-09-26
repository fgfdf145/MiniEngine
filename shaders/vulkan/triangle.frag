#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 0) const bool kAlphaMask = false;

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

// The scene behind transmissive surfaces (VulkanTransmissionCopyPass): pre-exposed, full mip chain.
layout(set = 0, binding = 18) uniform sampler2D transmissionCopy;

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
    if (material.diffuseTransmission.a > 0.0)
    {
        // KHR_materials_diffuse_transmission: the factor times its map's A, the colour times its map,
        // attenuated through the volume's thickness (times the node's mean scale) when it has one.
        specular.diffuseTransmissionFactor = clamp(
            material.diffuseTransmission.a * texture(diffuseTransmissionTexture, MaterialSlotUv(material, fragDrawSlot, 25u, fragTexCoord, fragTexCoord1)).a,
            0.0, 1.0);
        vec3 color = material.diffuseTransmission.rgb *
                     texture(diffuseTransmissionColorTexture, MaterialSlotUv(material, fragDrawSlot, 26u, fragTexCoord, fragTexCoord1)).rgb;
        float thickness = material.transmissionFactors.y * texture(thicknessTexture, MaterialSlotUv(material, fragDrawSlot, 24u, fragTexCoord, fragTexCoord1)).g;
        float distance = DiffuseTransmissionDistance(thickness, fragModelScale * material.volumeScale.xyz);
        specular.diffuseTransmissionColor = ApplyVolumeAttenuation(color, distance, material.attenuationColor.rgb, material.transmissionFactors.z);
    }
    // The forward path has no screen-space reflection: the environment alone, specularly occluded.
    vec3 color = ShadeSurface(fragWorldPosition, N, geoNormal, V, albedo.rgb, metallic, roughness, ao, emissive, coat, sheen, anisotropy, specular, vec4(0.0));

    // The atmosphere between the surface and the camera, before blending: an approximation for
    // Blend items, exact for the forward-only order's opaque ones.
    color = ApplyAerialPerspective(color, fragWorldPosition) * ubo.exposure.x;

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance, pre-exposed (see pre_exposure.glsl).
    outColor = vec4(color, albedo.a);
}
