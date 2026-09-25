#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

enum class MaterialAlphaMode
{
    Opaque,
    Mask,
    Blend
};

std::optional<MaterialAlphaMode> ParseMaterialAlphaMode(std::string_view value);

// KHR_materials_ior accepts 0 (an infinite index) or anything from 1 up; values in between, and
// negative ones, are not indices of refraction and read as 1.5, the extension's default.
float SanitizeIor(float ior);

// The dielectric F0 KHR_materials_ior and KHR_materials_specular give, before the maps:
// min(((ior - 1) / (ior + 1))^2 * specularColor, 1) * specular. F90 is the specular factor.
void ComputeDielectricF0(float ior, const float specularColor[3], float specular, float f0[3]);
const char* ToString(MaterialAlphaMode mode);
float ClampMaterialAlphaValue(float value);
float ClampMaterialAlphaValue(float value, float fallback);
float ResolveMaterialCoverageAlpha(MaterialAlphaMode mode, float alpha, float cutoff);

struct MaterialPbrSurfaceSettings
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    // KHR_materials_clearcoat: the coat's weight and its perceptual roughness, both [0, 1]. A
    // factor of 0 means no coat.
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    // KHR_materials_sheen: the sheen's linear colour and perceptual roughness, all [0, 1]. A black
    // colour means no sheen.
    float sheenColorFactor[3] = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    // KHR_materials_anisotropy: the strength [0, 1] and the direction's rotation in radians,
    // counter-clockwise from the tangent. A strength of 0 means an isotropic base.
    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    // KHR_materials_ior: the dielectric's index of refraction; 1.5 gives the usual F0 of 0.04, 0
    // stands for an infinite index (F0 = 1).
    float ior = 1.5f;
    // KHR_materials_specular: scales the dielectric's F0 and F90 alike, and tints its F0.
    float specularFactor = 1.0f;
    float specularColorFactor[3] = {1.0f, 1.0f, 1.0f};
    // KHR_materials_clearcoat's clearcoatNormalTexture scale.
    float clearcoatNormalScale = 1.0f;
    // KHR_materials_iridescence: the thin film's weight, index of refraction and thickness range in
    // nanometres (the thickness map picks between them). A factor of 0 means no film.
    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMinimum = 100.0f;
    float iridescenceThicknessMaximum = 400.0f;
};

struct MaterialGraphNodePosition
{
    float x = 0.0f;
    float y = 0.0f;
};

struct MaterialShaderNodeLayout
{
    MaterialGraphNodePosition primarySurfaceNode{72.0f, 80.0f};
    MaterialGraphNodePosition blendNode{420.0f, 260.0f};
    MaterialGraphNodePosition secondarySurfaceNode{760.0f, 80.0f};
    MaterialGraphNodePosition outputNode{1120.0f, 240.0f};
};

enum class MaterialShaderNodeType : uint32_t
{
    Texture = 0,
    Scalar = 1,
    Color = 2,
    Surface = 3,
    Blend = 4,
    Output = 5
};

struct MaterialShaderNode
{
    uint32_t id = 0;
    MaterialShaderNodeType type = MaterialShaderNodeType::Texture;
    std::string name;
    MaterialGraphNodePosition position;
    float width = 0.0f;
    float height = 0.0f;
    std::string texturePath;
    float scalarValue = 1.0f;
    float colorValue[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    MaterialPbrSurfaceSettings pbr;
};

struct MaterialShaderLink
{
    uint32_t id = 0;
    uint32_t fromNodeId = 0;
    std::string fromSlot;
    uint32_t toNodeId = 0;
    std::string toSlot;
};

struct MaterialShaderGraph
{
    uint32_t nextNodeId = 1;
    uint32_t nextLinkId = 1;
    std::vector<MaterialShaderNode> nodes;
    std::vector<MaterialShaderLink> links;

    bool IsEmpty() const
    {
        return nodes.empty();
    }
};

struct MaterialTextureBlendGraph
{
    bool enabled = false;
    float blendFactor = 0.0f;
    std::string blendMaskTexturePath;
    std::string secondaryBaseColorTexturePath;
    std::string secondaryNormalTexturePath;
    std::string secondaryMetallicTexturePath;
    std::string secondaryRoughnessTexturePath;
    std::string secondaryOcclusionTexturePath;
    std::string secondaryEmissiveTexturePath;
};
}
