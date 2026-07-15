#pragma once

#include <scene_components.h>

#include <cstdint>
#include <string>
#include <vector>

struct RendererSharedState;

// Model import and material sidecar maintenance operations.
namespace ModelImportService
{
// Imports a model into its own folder (named after the model) under the
// destination directory. A .gltf's referenced companions are sorted into
// buffers/ and textures/ subfolders and its URIs rewritten to match. Returns
// the imported model path. Blocking; prefer StartAsyncImport from the UI
// thread.
std::string ImportModelIntoAssetDirectory(const std::string& sourcePath, const std::string& destinationDirectory);

// Runs ImportModelIntoAssetDirectory on a background thread via
// state.asyncImport. Throws if another import is still in flight.
void StartAsyncImport(RendererSharedState& state, const std::string& sourcePath, const std::string& destinationDirectory);

// Polls the in-flight import once per frame. On completion, reports the
// outcome (state.lastModelLoadError on failure) and refreshes the asset
// browser so the new files show up.
void PumpAsyncImport(RendererSharedState& state);

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
