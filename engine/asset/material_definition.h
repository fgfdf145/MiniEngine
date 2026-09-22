#pragma once

#include "model_loader.h"

#include <engine/scene/scene_components.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace me
{

std::filesystem::path BuildMaterialDefinitionPath(
    const std::filesystem::path& modelPath,
    uint32_t materialIndex);
// Every "<stem>_<index>.material.yaml" next to the model, whatever the index.
std::vector<std::filesystem::path> FindMaterialDefinitionFiles(const std::filesystem::path& modelPath);
// The <index> of a file FindMaterialDefinitionFiles returned for this model.
std::optional<uint32_t> MaterialDefinitionIndex(
    const std::filesystem::path& modelPath,
    const std::filesystem::path& definitionPath);
// The material name a definition was saved for; std::nullopt when the file
// cannot be read or records none.
std::optional<std::string> ReadMaterialDefinitionName(const std::filesystem::path& definitionPath);
ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material);
void ApplyImportedMaterialInfo(const ModelImportedMaterialInfo& source, ModelMaterialData& destination);
YAML::Node SerializeMaterialDefinition(const ModelImportedMaterialInfo& material);
bool LoadMaterialDefinition(
    const std::filesystem::path& path,
    ModelImportedMaterialInfo& material,
    std::string& warning);
}
