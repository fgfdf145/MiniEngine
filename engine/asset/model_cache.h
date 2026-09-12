#pragma once

#include "model_loader.h"

#include <engine/scene/scene_components.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace me
{

// Parsed model data is the largest thing the editor holds on the CPU. One
// Sponza-scale bundle is hundreds of megabytes, and without a bound every
// model ever loaded stays resident until the process exits.
inline constexpr size_t kDefaultModelCacheBudgetBytes = 1ull << 30; // 1 GiB

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

// Vertices plus indices. Ignores the material and name strings: they are
// kilobytes against a mesh's megabytes, and an estimate that is stable and
// cheap beats one that is exact.
size_t EstimateBytes(const LoadedModelData& data);

// Total bytes currently held. Exists for tests and diagnostics.
size_t TotalBytes();

// Evicts until the total is within budget. Entries whose key is in `liveKeys`
// are never evicted, whatever the budget: the scene still references them, and
// evicting one only forces a synchronous reload on the next renderable
// rebuild. Everything else goes least-recently-used first. `liveKeys` are
// normalized with the cache's own key rule, so callers may pass raw paths.
void Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes);
}
}
