#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MaterialPbrSurfaceSettings
{
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
};

struct MaterialGraphNodePosition
{
    float x = 0.0f;
    float y = 0.0f;
};

struct MaterialShaderNodeLayout
{
    MaterialGraphNodePosition primarySurfaceNode{ 72.0f, 80.0f };
    MaterialGraphNodePosition blendNode{ 420.0f, 260.0f };
    MaterialGraphNodePosition secondarySurfaceNode{ 760.0f, 80.0f };
    MaterialGraphNodePosition outputNode{ 1120.0f, 240.0f };
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
    std::string texturePath;
    float scalarValue = 1.0f;
    float colorValue[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
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
