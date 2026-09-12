#pragma once

#include "model_loader.h"

#include <engine/scene/scene_components.h>

#include <memory>
#include <string>
#include <vector>

namespace me
{

// Thread-safe cache of parsed model data, keyed by source path.
// Written by background loader threads; read by the main thread.
namespace ModelCache
{
bool IsCached(const std::string& path);

// Const on purpose: the cache hands out a shared handle, and writing through
// it from one caller while another reads is how the two mutation sites used to
// work. Mutation goes through the named entry points below.
std::shared_ptr<const LoadedModelData> Get(const std::string& path);

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data);

// Apply an edited material back into the cached model. No-ops when the path is
// not cached or the index is out of range, matching the guards the call sites
// used to carry themselves.
void UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material);
void UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials);

// Removes the cached entry for `path`. If `path` is a directory, every cached
// model under it is removed as well. Call before deleting assets on disk so
// stale data is not served for a re-imported file at the same path.
void Invalidate(const std::string& path);
}
}
