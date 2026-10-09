#ifndef MATERIAL_LAYERS_SLANG
#define MATERIAL_LAYERS_SLANG

// The layer maps of the material set (the clearcoat, sheen, anisotropy and specular extensions'
// textures) and what gbuffer.frag and triangle.frag both make of them. Each map multiplies its
// factor; an absent one is bound as white, the anisotropy map as (1, 0.5, 1) and the coat normal
// as the flat normal, so the factors alone apply. Needs material_common.slang, gbuffer_common.slang,
// normal_map.slang, material_uv.slang and anisotropy_common.slang.

[[vk::binding(13, 1)]] Texture2D clearcoatTexture : register(t13, space1);                // R
[[vk::binding(MATERIAL_SAMPLER(13), 1)]] SamplerState clearcoatTextureSampler : register(s77, space1);
[[vk::binding(14, 1)]] Texture2D clearcoatRoughnessTexture : register(t14, space1);       // G
[[vk::binding(MATERIAL_SAMPLER(14), 1)]] SamplerState clearcoatRoughnessTextureSampler : register(s78, space1);
[[vk::binding(15, 1)]] Texture2D sheenColorTexture : register(t15, space1);               // RGB, sRGB
[[vk::binding(MATERIAL_SAMPLER(15), 1)]] SamplerState sheenColorTextureSampler : register(s79, space1);
[[vk::binding(16, 1)]] Texture2D sheenRoughnessTexture : register(t16, space1);           // A
[[vk::binding(MATERIAL_SAMPLER(16), 1)]] SamplerState sheenRoughnessTextureSampler : register(s80, space1);
[[vk::binding(17, 1)]] Texture2D anisotropyTexture : register(t17, space1);               // RG direction, B strength
[[vk::binding(MATERIAL_SAMPLER(17), 1)]] SamplerState anisotropyTextureSampler : register(s81, space1);
[[vk::binding(18, 1)]] Texture2D specularTexture : register(t18, space1);                 // A
[[vk::binding(MATERIAL_SAMPLER(18), 1)]] SamplerState specularTextureSampler : register(s82, space1);
[[vk::binding(19, 1)]] Texture2D specularColorTexture : register(t19, space1);            // RGB, sRGB
[[vk::binding(MATERIAL_SAMPLER(19), 1)]] SamplerState specularColorTextureSampler : register(s83, space1);
[[vk::binding(20, 1)]] Texture2D clearcoatNormalTexture : register(t20, space1);          // tangent-space normal
[[vk::binding(MATERIAL_SAMPLER(20), 1)]] SamplerState clearcoatNormalTextureSampler : register(s84, space1);
[[vk::binding(21, 1)]] Texture2D iridescenceTexture : register(t21, space1);              // R
[[vk::binding(MATERIAL_SAMPLER(21), 1)]] SamplerState iridescenceTextureSampler : register(s85, space1);
[[vk::binding(22, 1)]] Texture2D iridescenceThicknessTexture : register(t22, space1);     // G
[[vk::binding(MATERIAL_SAMPLER(22), 1)]] SamplerState iridescenceThicknessTextureSampler : register(s86, space1);
[[vk::binding(23, 1)]] Texture2D transmissionTexture : register(t23, space1);             // R
[[vk::binding(MATERIAL_SAMPLER(23), 1)]] SamplerState transmissionTextureSampler : register(s87, space1);
[[vk::binding(24, 1)]] Texture2D thicknessTexture : register(t24, space1);                // G
[[vk::binding(MATERIAL_SAMPLER(24), 1)]] SamplerState thicknessTextureSampler : register(s88, space1);
[[vk::binding(25, 1)]] Texture2D diffuseTransmissionTexture : register(t25, space1);      // A
[[vk::binding(MATERIAL_SAMPLER(25), 1)]] SamplerState diffuseTransmissionTextureSampler : register(s89, space1);
[[vk::binding(26, 1)]] Texture2D diffuseTransmissionColorTexture : register(t26, space1); // RGB, sRGB
[[vk::binding(MATERIAL_SAMPLER(26), 1)]] SamplerState diffuseTransmissionColorTextureSampler : register(s90, space1);

struct MaterialLayers
{
    // SHADING_FLAG_* bits (gbuffer_common.slang) the material carries.
    uint flags;
    float coatFactor;
    // Perceptual, not yet floored or filtered.
    float coatRoughness;
    // The geometric normal unless the coat has its own map.
    float3 coatNormal;
    float3 sheenColor;
    float sheenRoughness;
    // World space, in the surface of the shading normal.
    float3 anisotropyTangent;
    float anisotropyStrength;
    // The dielectric's F0 and F90: 0.04 and 1 unless the specular flag is set.
    float3 dielectricF0;
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
MaterialLayers EvaluateMaterialLayers(MaterialData material, uint drawSlot, float2 uv0, float2 uv1, float3x3 TBN, float3 N)
{
    MaterialLayers layers;
    layers.flags = material.shadingModel.x;

    layers.coatFactor = 0.0;
    layers.coatRoughness = 0.0;
    layers.coatNormal = ColumnOf(TBN, 2);
    if (HasShadingFlag(layers.flags, SHADING_FLAG_CLEARCOAT))
    {
        layers.coatFactor = clamp(material.clearcoatFactors.x * MaterialTexture(clearcoatTexture, MaterialSlotUv(material, drawSlot, 13u, uv0, uv1)).r, 0.0, 1.0);
        layers.coatRoughness = clamp(material.clearcoatFactors.y * MaterialTexture(clearcoatRoughnessTexture, MaterialSlotUv(material, drawSlot, 14u, uv0, uv1)).g, 0.0, 1.0);
        if (HasShadingFlag(layers.flags, SHADING_FLAG_COAT_NORMAL))
        {
            float3 coatSample = DecodeNormalMap(MaterialTexture(clearcoatNormalTexture, MaterialSlotUv(material, drawSlot, 20u, uv0, uv1)));
            coatSample.xy = RotateMaterialTangentXy(material, drawSlot, 20u, coatSample.xy);
            coatSample.xy *= material.clearcoatFactors.z;
            layers.coatNormal = normalize(mul(TBN, coatSample));
        }
    }

    layers.sheenColor = float3(0.0);
    layers.sheenRoughness = 0.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_SHEEN))
    {
        layers.sheenColor = clamp(material.sheenFactors.rgb * MaterialTexture(sheenColorTexture, MaterialSlotUv(material, drawSlot, 15u, uv0, uv1)).rgb, 0.0, 1.0);
        layers.sheenRoughness = clamp(material.sheenFactors.a * MaterialTexture(sheenRoughnessTexture, MaterialSlotUv(material, drawSlot, 16u, uv0, uv1)).a, 0.0, 1.0);
    }

    layers.anisotropyTangent = ColumnOf(TBN, 0);
    layers.anisotropyStrength = 0.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_ANISOTROPY))
    {
        float3 sampled = MaterialTexture(anisotropyTexture, MaterialSlotUv(material, drawSlot, 17u, uv0, uv1)).rgb;
        float2 direction = RotateMaterialTangentXy(
            material, drawSlot, 17u, AnisotropyDirection(sampled.rg, material.anisotropyFactors.y, material.anisotropyFactors.z));
        float3 tangent = mul(TBN, float3(direction, 0.0));
        // Into the surface of the shading normal, which the normal map may have tilted.
        tangent -= N * dot(N, tangent);
        float length2 = dot(tangent, tangent);
        if (length2 > 1e-8)
        {
            layers.anisotropyTangent = tangent * rsqrt(length2);
            layers.anisotropyStrength = clamp(material.anisotropyFactors.x * sampled.b, 0.0, 1.0);
        }
    }

    // KHR_materials_ior and KHR_materials_specular: F0 = min(ior F0 * colour, 1) * specular,
    // F90 = specular, the factors times their maps.
    layers.dielectricF0 = float3(0.04);
    layers.dielectricF90 = 1.0;
    if (HasShadingFlag(layers.flags, SHADING_FLAG_SPECULAR))
    {
        float specular = clamp(material.specularFactors.a * MaterialTexture(specularTexture, MaterialSlotUv(material, drawSlot, 18u, uv0, uv1)).a, 0.0, 1.0);
        layers.dielectricF0 = min(material.specularFactors.rgb * MaterialTexture(specularColorTexture, MaterialSlotUv(material, drawSlot, 19u, uv0, uv1)).rgb, float3(1.0)) * specular;
        layers.dielectricF90 = specular;
    }

    layers.iridescenceFactor = 0.0;
    layers.iridescenceThickness = 0.0;
    if (material.iridescenceFactors.x > 0.0)
    {
        layers.iridescenceFactor = clamp(material.iridescenceFactors.x * MaterialTexture(iridescenceTexture, MaterialSlotUv(material, drawSlot, 21u, uv0, uv1)).r, 0.0, 1.0);
        layers.iridescenceThickness =
            lerp(material.iridescenceFactors.z, material.iridescenceFactors.w, MaterialTexture(iridescenceThicknessTexture, MaterialSlotUv(material, drawSlot, 22u, uv0, uv1)).g);
    }
    return layers;
}

#endif
