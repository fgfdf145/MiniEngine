#ifndef MATERIAL_LAYERS_GLSL
#define MATERIAL_LAYERS_GLSL

// The layer maps of the material set (the clearcoat, sheen, anisotropy and specular extensions'
// textures) and what gbuffer.frag and triangle.frag both make of them. Each map multiplies its
// factor; an absent one is bound as white, the anisotropy map as (1, 0.5, 1) and the coat normal
// as the flat normal, so the factors alone apply. Needs material_common.glsl, gbuffer_common.glsl,
// normal_map.glsl, material_uv.glsl and anisotropy_common.glsl.

layout(set = 1, binding = 13) uniform sampler2D clearcoatTexture;          // R
layout(set = 1, binding = 14) uniform sampler2D clearcoatRoughnessTexture; // G
layout(set = 1, binding = 15) uniform sampler2D sheenColorTexture;         // RGB, sRGB
layout(set = 1, binding = 16) uniform sampler2D sheenRoughnessTexture;     // A
layout(set = 1, binding = 17) uniform sampler2D anisotropyTexture;         // RG direction, B strength
layout(set = 1, binding = 18) uniform sampler2D specularTexture;           // A
layout(set = 1, binding = 19) uniform sampler2D specularColorTexture;      // RGB, sRGB
layout(set = 1, binding = 20) uniform sampler2D clearcoatNormalTexture;    // tangent-space normal
layout(set = 1, binding = 21) uniform sampler2D iridescenceTexture;        // R
layout(set = 1, binding = 22) uniform sampler2D iridescenceThicknessTexture; // G

struct MaterialLayers
{
    // SHADING_FLAG_* bits (gbuffer_common.glsl) the material carries.
    uint flags;
    float coatFactor;
    // Perceptual, not yet floored or filtered.
    float coatRoughness;
    // The geometric normal unless the coat has its own map.
    vec3 coatNormal;
    vec3 sheenColor;
    float sheenRoughness;
    // World space, in the surface of the shading normal.
    vec3 anisotropyTangent;
    float anisotropyStrength;
    // The dielectric's F0 and F90: 0.04 and 1 unless the specular flag is set.
    vec3 dielectricF0;
    float dielectricF90;
    // The thin film (forward pass only): its weight and thickness in nanometres.
    float iridescenceFactor;
    float iridescenceThickness;
};

bool HasShadingFlag(uint flags, uint flag)
{
    return (flags & flag) != 0u;
}

// TBN is the geometric tangent frame (tangent, bitangent, geometric normal), N the shading normal.
MaterialLayers EvaluateMaterialLayers(MaterialData material, uint drawSlot, vec2 uv0, vec2 uv1, mat3 TBN, vec3 N)
{
    MaterialLayers layers;
    layers.flags = material.shadingModel.x;

    layers.coatFactor = 0.0;
    layers.coatRoughness = 0.0;
    layers.coatNormal = TBN[2];
    if (HasShadingFlag(layers.flags, SHADING_FLAG_CLEARCOAT))
    {
        layers.coatFactor = clamp(material.clearcoatFactors.x * texture(clearcoatTexture, MaterialSlotUv(material, drawSlot, 13u, uv0, uv1)).r, 0.0, 1.0);
        layers.coatRoughness = clamp(material.clearcoatFactors.y * texture(clearcoatRoughnessTexture, MaterialSlotUv(material, drawSlot, 14u, uv0, uv1)).g, 0.0, 1.0);
        if (HasShadingFlag(layers.flags, SHADING_FLAG_COAT_NORMAL))
        {
            vec3 coatSample = DecodeNormalMap(texture(clearcoatNormalTexture, MaterialSlotUv(material, drawSlot, 20u, uv0, uv1)));
            coatSample.xy = RotateMaterialTangentXy(material, drawSlot, 20u, coatSample.xy);
            coatSample.xy *= material.clearcoatFactors.z;
            layers.coatNormal = normalize(TBN * coatSample);
        }
    }

    layers.sheenColor = vec3(0.0);
    layers.sheenRoughness = 0.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_SHEEN))
    {
        layers.sheenColor = clamp(material.sheenFactors.rgb * texture(sheenColorTexture, MaterialSlotUv(material, drawSlot, 15u, uv0, uv1)).rgb, 0.0, 1.0);
        layers.sheenRoughness = clamp(material.sheenFactors.a * texture(sheenRoughnessTexture, MaterialSlotUv(material, drawSlot, 16u, uv0, uv1)).a, 0.0, 1.0);
    }

    layers.anisotropyTangent = TBN[0];
    layers.anisotropyStrength = 0.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_ANISOTROPY))
    {
        vec3 sampled = texture(anisotropyTexture, MaterialSlotUv(material, drawSlot, 17u, uv0, uv1)).rgb;
        vec2 direction = RotateMaterialTangentXy(
            material, drawSlot, 17u, AnisotropyDirection(sampled.rg, material.anisotropyFactors.y, material.anisotropyFactors.z));
        vec3 tangent = TBN * vec3(direction, 0.0);
        // Into the surface of the shading normal, which the normal map may have tilted.
        tangent -= N * dot(N, tangent);
        float length2 = dot(tangent, tangent);
        if (length2 > 1e-8)
        {
            layers.anisotropyTangent = tangent * inversesqrt(length2);
            layers.anisotropyStrength = clamp(material.anisotropyFactors.x * sampled.b, 0.0, 1.0);
        }
    }

    // KHR_materials_ior and KHR_materials_specular: F0 = min(ior F0 * colour, 1) * specular,
    // F90 = specular, the factors times their maps.
    layers.dielectricF0 = vec3(0.04);
    layers.dielectricF90 = 1.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_SPECULAR))
    {
        float specular = clamp(material.specularFactors.a * texture(specularTexture, MaterialSlotUv(material, drawSlot, 18u, uv0, uv1)).a, 0.0, 1.0);
        layers.dielectricF0 = min(material.specularFactors.rgb * texture(specularColorTexture, MaterialSlotUv(material, drawSlot, 19u, uv0, uv1)).rgb, vec3(1.0)) * specular;
        layers.dielectricF90 = specular;
    }

    layers.iridescenceFactor = 0.0;
    layers.iridescenceThickness = 0.0;
    if (material.iridescenceFactors.x > 0.0)
    {
        layers.iridescenceFactor = clamp(material.iridescenceFactors.x * texture(iridescenceTexture, MaterialSlotUv(material, drawSlot, 21u, uv0, uv1)).r, 0.0, 1.0);
        layers.iridescenceThickness =
            mix(material.iridescenceFactors.z, material.iridescenceFactors.w, texture(iridescenceThicknessTexture, MaterialSlotUv(material, drawSlot, 22u, uv0, uv1)).g);
    }
    return layers;
}

#endif
