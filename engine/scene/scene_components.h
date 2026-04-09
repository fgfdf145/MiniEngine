#pragma once

#include "material_graph.h"
#include "world_units.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

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
