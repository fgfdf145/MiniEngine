#pragma once

#include "material_graph.h"
#include "world_units.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

enum class LightType : uint32_t
{
    Directional = 0, // Global sun-like light, uses transform rotation for direction
    Point       = 1, // Omnidirectional point light
    Spot        = 2, // Cone spotlight
    Area        = 3, // Rectangular area light (approximate)
    Ambient     = 4  // Global ambient (no position or direction)
};

struct LightComponent
{
    LightType type = LightType::Point;
    glm::vec3 color{ 1.0f, 1.0f, 1.0f };
    float intensity = 1000.0f;                // Lumens (point/spot/area) or lux (directional)
    float range = 10.0f;                      // Effective range in meters
    float spotInnerAngleDegrees = 15.0f;     // Spot inner cone half-angle
    float spotOuterAngleDegrees = 30.0f;     // Spot outer cone half-angle
    glm::vec2 areaSize{ 1.0f, 1.0f };        // Area light width x height in meters
};

struct ModelImportedMaterialInfo
{
    std::string name;
    std::string baseColorTexturePath;
    std::string normalTexturePath;
    std::string metallicTexturePath;
    std::string roughnessTexturePath;
    std::string occlusionTexturePath;
    std::string emissiveTexturePath;
    MaterialPbrSurfaceSettings pbr;
    MaterialTextureBlendGraph blendGraph;
    MaterialShaderGraph shaderGraph;
};

struct ModelImportedSubmeshInfo
{
    std::string name;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    bool hasTexCoords = false;
    bool hasNormals = false;
    bool hasTangents = false;
};

struct TagComponent
{
    std::string name = "Cube";
};

struct TransformComponent
{
    glm::vec3 translation{ 0.0f, 0.0f, 0.0f };
    glm::vec3 rotationDegrees{ 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
};

struct ModelComponent
{
    std::string sourcePath;
    std::string displayName = "Cube";
    std::string baseColorTextureOverridePath;
    uint32_t submeshCount = 1;
    glm::vec3 minBounds = WorldUnits::kDefaultCubeMinBoundsMeters;
    glm::vec3 maxBounds = WorldUnits::kDefaultCubeMaxBoundsMeters;
    bool hasBounds = true;
    std::vector<ModelImportedMaterialInfo> importedMaterials;
    std::vector<ModelImportedSubmeshInfo> importedSubmeshes;
};
