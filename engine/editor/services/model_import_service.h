#pragma once

#include <engine/asset/kn5_importer.h>
#include <engine/asset/model_import_target.h>
#include <engine/scene/scene_components.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace me
{

struct RendererSharedState;

// Model import and material sidecar maintenance operations.
namespace ModelImportService
{
// Imports a model into its own folder (named after the model) under the
// destination directory. A .gltf's referenced companions are sorted into
// buffers/ and textures/ subfolders and its URIs rewritten to match; an
// Assetto Corsa .kn5 is converted into a glTF bundle laid out the same way,
// and the returned path is that glTF, converted as `kn5Options` asks (ignored
// for other formats). `policy`
// decides what happens when that folder already holds files; see
// ModelImportTarget. `progress` hears the overall fraction done. Returns the
// imported model path. Blocking; prefer StartAsyncImport from the UI thread.
std::string ImportModelIntoAssetDirectory(
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy = ImportConflictPolicy::FailIfExists,
    const Kn5ImportOptions& kn5Options = {},
    const ImportProgressCallback& progress = {});

// Runs ImportModelIntoAssetDirectory on a background thread via
// state.asyncImport, whose Progress() follows it. Throws if another import is
// still in flight.
void StartAsyncImport(
    RendererSharedState& state,
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    ImportConflictPolicy policy,
    const Kn5ImportOptions& kn5Options = {});

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
// Saves the materials at `indices` (all of them when empty) as sidecar .material.yaml files and
// shows them on every entity drawing the model. The slots in `restoredIndices` are back to the
// material their import made: their sidecars are removed instead.
void UpdateImportedModelMaterialDefinitions(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<ModelImportedMaterialInfo>& materials,
    const std::vector<uint32_t>& indices = {},
    const std::vector<uint32_t>& restoredIndices = {});
// Shows edited materials, each with its slot index, on every entity drawing the model, writing
// nothing to disk.
void PreviewImportedModelMaterials(
    RendererSharedState& state,
    const std::string& modelPathString,
    const std::vector<std::pair<uint32_t, ModelImportedMaterialInfo>>& materials);
// Drops previewed edits: the model is read again from disk, sidecars included.
void RevertImportedModelMaterials(RendererSharedState& state, const std::string& modelPathString);
}
}
