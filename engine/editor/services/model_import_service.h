#pragma once

#include <scene_components.h>

#include <cstdint>
#include <string>
#include <vector>

struct RendererSharedState;

// Model import and material sidecar maintenance operations.
namespace ModelImportService
{
// Copies a model file (plus companion textures/buffers for .gltf) into the
// asset directory. Returns the imported model path.
std::string ImportModelIntoAssetDirectory(const std::string& sourcePath, const std::string& destinationDirectory);
void DeleteAssetPath(const std::string& path);
void PasteAsset(const std::string& sourcePath, const std::string& destinationDirectory);
void UpdateImportedMaterialDefinition(
    RendererSharedState& state,
    const std::string& modelPath,
    uint32_t materialIndex,
    const ModelImportedMaterialInfo& material
);
void UpdateImportedModelMaterialDefinitions(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<ModelImportedMaterialInfo>& materials
);
}
