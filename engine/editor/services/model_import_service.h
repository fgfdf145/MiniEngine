#pragma once

#include <engine/asset/model_import_target.h>
#include <engine/scene/scene_components.h>

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

struct RendererSharedState;

// Model import and material sidecar maintenance operations.
namespace ModelImportService
{
// Imports a model into its own folder (named after the model) under the
// destination directory. A .gltf's referenced companions are sorted into
// buffers/ and textures/ subfolders and its URIs rewritten to match. `policy`
// decides what happens when that folder already holds files; see
// ModelImportTarget. Returns the imported model path. Blocking; prefer
// StartAsyncImport from the UI thread.
std::string ImportModelIntoAssetDirectory(
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy = ImportConflictPolicy::FailIfExists);

// Runs ImportModelIntoAssetDirectory on a background thread via
// state.asyncImport. Throws if another import is still in flight.
void StartAsyncImport(
    RendererSharedState& state,
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy);

// Polls the in-flight import once per frame. On completion, reports the
// outcome (state.lastModelLoadError on failure) and refreshes the asset
// browser so the new files show up.
void PumpAsyncImport(RendererSharedState& state);

// Deletes a file or folder. A model file takes its material definitions with
// it, after the model itself is gone.
void DeleteAssetPath(const std::string& path);

// Copies a file or folder into the destination directory, never overwriting:
// a taken name becomes "<name>_copy". A model file is copied with its
// companions and material definitions into its own folder, like an import.
void PasteAsset(const std::string& sourcePath, const std::string& destinationDirectory);

// Points scene entities that referenced a renamed file, or anything inside a
// renamed folder, at the new path.
void OnAssetRenamed(RendererSharedState& state, const std::string& oldPath, const std::string& newPath);
void UpdateImportedModelMaterialDefinitions(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<ModelImportedMaterialInfo>& materials);
}
}
