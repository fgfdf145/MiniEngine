#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 0) const bool kAlphaMask = false;

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "normal_map.glsl"
#include "material_common.glsl"
// SHADING_MODEL_* for the material's shading model id.
#include "gbuffer_common.glsl"
#include "specular_aa.glsl"

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

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in vec4 fragWorldTangent;
layout(location = 4) in vec3 fragWorldPosition;
layout(location = 7) flat in uint fragDrawSlot;

layout(location = 0) out vec4 outColor;

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    MaterialData material = materialData.materials[fragDrawSlot];

    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, material.nodeGraphFactors.y, clamp(material.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    vec4 primaryBaseColor = texture(baseColorTexture, fragTexCoord);
    vec4 secondaryBaseColor = texture(secondaryBaseColorTexture, fragTexCoord);
    vec4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * material.baseColorFactor;

    if (kAlphaMask && albedo.a < material.alphaCutoff)
        discard;

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

    vec3 nrmPrimary = DecodeNormalMap(texture(normalTexture, fragTexCoord));
    vec3 nrmSecondary = DecodeNormalMap(texture(secondaryNormalTexture, fragTexCoord));
    vec3 nrmSample = normalize(mix(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= material.surfaceFactors.z; // normal scale
    vec3 N = normalize(TBN * nrmSample);

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = mix(
        texture(metallicTexture, fragTexCoord).b,
        texture(secondaryMetallicTexture, fragTexCoord).b,
        blendWeight);
    float roughnessSample = mix(
        texture(roughnessTexture, fragTexCoord).g,
        texture(secondaryRoughnessTexture, fragTexCoord).g,
        blendWeight);
    float aoSample = mix(
        texture(occlusionTexture, fragTexCoord).r,
        texture(secondaryOcclusionTexture, fragTexCoord).r,
        blendWeight);
    vec3 emissiveSample = mix(
        texture(emissiveTexture, fragTexCoord).rgb,
        texture(secondaryEmissiveTexture, fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(material.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(material.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    // As gbuffer.frag: both variations here, in uniform control flow.
    roughness = FilterRoughnessForSpecularAA(roughness, NormalVariation(N));
    float coatNormalVariation = NormalVariation(geoNormal);
    float ao = mix(1.0, aoSample, clamp(material.surfaceFactors.w, 0.0, 1.0));

    vec3 V = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);

    // ---- Shade ------------------------------------------------------------
    // ShadeSurface is the ambient, direct and shadow arithmetic deferred_lighting.frag also runs;
    // it lives in pbr_common.glsl so the forward comparison path and the deferred path cannot
    // drift.
    vec3 emissive = emissiveSample * material.emissiveFactor;
    // The forward path reads the coat from the material, where the deferred path reads it from GB5.
    CoatParams coat = NoCoat();
    SheenParams sheen = NoSheen();
    if (material.shadingModel.x == SHADING_MODEL_CLEARCOAT)
    {
        coat.factor = clamp(material.clearcoatFactors.x, 0.0, 1.0);
        coat.roughness = FilterRoughnessForSpecularAA(clamp(material.clearcoatFactors.y, 0.04, 1.0), coatNormalVariation);
        coat.normal = geoNormal;
    }
    else if (material.shadingModel.x == SHADING_MODEL_SHEEN)
    {
        sheen.color = clamp(material.sheenFactors.rgb, 0.0, 1.0);
        sheen.roughness = clamp(material.sheenFactors.a, 0.04, 1.0);
    }
    vec3 color = ShadeSurface(fragWorldPosition, N, geoNormal, V, albedo.rgb, metallic, roughness, ao, emissive, coat, sheen);

    // The atmosphere between the surface and the camera, before blending: an approximation for
    // Blend items, exact for the forward-only order's opaque ones.
    color = ApplyAerialPerspective(color, fragWorldPosition);

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance.
    outColor = vec4(color, albedo.a);
}
