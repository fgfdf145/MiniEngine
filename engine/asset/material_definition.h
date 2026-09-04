#pragma once

#include "model_loader.h"

#include <engine/scene/scene_components.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace me
{

std::filesystem::path BuildMaterialDefinitionPath(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex);
ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material);
void ApplyImportedMaterialInfo(const ModelImportedMaterialInfo& source, ModelMaterialData& destination);
YAML::Node SerializeMaterialDefinition(const ModelImportedMaterialInfo& material);
bool LoadMaterialDefinition(
    const std::filesystem::path& path,
    ModelImportedMaterialInfo& material,
    std::string& warning);
}
