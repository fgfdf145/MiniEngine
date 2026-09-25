#ifndef MATERIAL_LAYERS_GLSL
#define MATERIAL_LAYERS_GLSL

// The layer maps of the material set (KHR_materials_clearcoat, _sheen and _anisotropy textures)
// and what gbuffer.frag and triangle.frag both make of them. Each map multiplies its factor; an
// absent one is bound as white, the anisotropy map as (1, 0.5, 1), so the factors alone apply.
// Needs material_common.glsl, gbuffer_common.glsl and anisotropy_common.glsl.

layout(set = 1, binding = 13) uniform sampler2D clearcoatTexture;          // R
layout(set = 1, binding = 14) uniform sampler2D clearcoatRoughnessTexture; // G
layout(set = 1, binding = 15) uniform sampler2D sheenColorTexture;         // RGB, sRGB
layout(set = 1, binding = 16) uniform sampler2D sheenRoughnessTexture;     // A
layout(set = 1, binding = 17) uniform sampler2D anisotropyTexture;         // RG direction, B strength

struct MaterialLayers
{
    // The layer the material's shading model id names, and whether its base is anisotropic.
    uint layer;
    bool anisotropic;
    float coatFactor;
    // Perceptual, not yet floored or filtered.
    float coatRoughness;
    vec3 sheenColor;
    float sheenRoughness;
    // World space, in the surface of the shading normal.
    vec3 anisotropyTangent;
    float anisotropyStrength;
};

// TBN is the geometric tangent frame (tangent, bitangent, geometric normal), N the shading normal.
MaterialLayers EvaluateMaterialLayers(MaterialData material, vec2 uv, mat3 TBN, vec3 N)
{
    MaterialLayers layers;
    layers.layer = material.shadingModel.x & SHADING_MODEL_LAYER_MASK;
    layers.anisotropic = (material.shadingModel.x & SHADING_MODEL_ANISOTROPY_BIT) != 0u;

    layers.coatFactor = 0.0;
    layers.coatRoughness = 0.0;
    if (layers.layer == SHADING_MODEL_CLEARCOAT)
    {
        layers.coatFactor = clamp(material.clearcoatFactors.x * texture(clearcoatTexture, uv).r, 0.0, 1.0);
        layers.coatRoughness = clamp(material.clearcoatFactors.y * texture(clearcoatRoughnessTexture, uv).g, 0.0, 1.0);
    }

    layers.sheenColor = vec3(0.0);
    layers.sheenRoughness = 0.0;
    if (layers.layer == SHADING_MODEL_SHEEN)
    {
        layers.sheenColor = clamp(material.sheenFactors.rgb * texture(sheenColorTexture, uv).rgb, 0.0, 1.0);
        layers.sheenRoughness = clamp(material.sheenFactors.a * texture(sheenRoughnessTexture, uv).a, 0.0, 1.0);
    }

    layers.anisotropyTangent = TBN[0];
    layers.anisotropyStrength = 0.0;
    if (layers.anisotropic)
    {
        vec3 sampled = texture(anisotropyTexture, uv).rgb;
        vec2 direction = AnisotropyDirection(sampled.rg, material.anisotropyFactors.y, material.anisotropyFactors.z);
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
    return layers;
}

#endif
