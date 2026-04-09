#pragma once

#include <scene_components.h>

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>
#include <string_view>

struct MaterialGraphCompileResult
{
    bool success = false;
    std::string message;
};

const char* ToString(MaterialShaderNodeType type);
bool TryParseMaterialShaderNodeType(std::string_view value, MaterialShaderNodeType& type);

MaterialShaderGraph BuildDefaultMaterialShaderGraph(
    const std::string& materialName,
    const std::string& baseColorTexturePath,
    const std::string& normalTexturePath,
    const std::string& metallicTexturePath,
    const std::string& roughnessTexturePath,
    const std::string& occlusionTexturePath,
    const std::string& emissiveTexturePath,
    const MaterialPbrSurfaceSettings& pbr,
    const MaterialTextureBlendGraph& blendGraph,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout = std::nullopt
);

void EnsureMaterialShaderGraph(
    const std::string& materialName,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout,
    ModelImportedMaterialInfo& material
);

YAML::Node SerializeMaterialShaderGraph(const MaterialShaderGraph& graph);
bool DeserializeMaterialShaderGraph(
    const YAML::Node& shaderGraphNode,
    const std::string& materialName,
    const std::optional<MaterialShaderNodeLayout>& legacyLayout,
    ModelImportedMaterialInfo& material
);

MaterialGraphCompileResult CompileMaterialShaderGraph(ModelImportedMaterialInfo& material);
