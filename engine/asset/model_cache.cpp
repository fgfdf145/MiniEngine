#include "model_cache.h"

#include "material_definition.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace me
{

namespace
{
std::mutex s_modelCacheMutex;

struct CacheEntry
{
    std::shared_ptr<LoadedModelData> data;
    size_t bytes = 0;
    uint64_t lastAccess = 0;
};

std::unordered_map<std::string, CacheEntry> s_modelCache;

// Monotonic, not a clock: cheaper, and it makes eviction order deterministic
// in tests.
uint64_t s_accessCounter = 0;

// Collapses separator and relative-vs-absolute variants of the same file into
// a single cache key, so "assets/a.glb" and "assets\\a.glb" don't produce
// duplicate entries.
std::string NormalizeKey(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        normalized = std::filesystem::absolute(path, ec);
        if (ec)
        {
            normalized = path;
        }
        normalized = normalized.lexically_normal();
    }
    return normalized.string();
}
}

namespace ModelCache
{
bool IsCached(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end())
    {
        return false;
    }
    it->second.lastAccess = ++s_accessCounter;
    return true;
}

std::shared_ptr<const LoadedModelData> Get(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end())
    {
        return nullptr;
    }
    it->second.lastAccess = ++s_accessCounter;
    return it->second.data;
}

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data)
{
    const std::string key = NormalizeKey(path);
    const size_t bytes = data ? EstimateBytes(*data) : 0;
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    s_modelCache[key] = CacheEntry{std::move(data), bytes, ++s_accessCounter};
}

void UpdateMaterial(const std::string& path, uint32_t materialIndex, const ModelImportedMaterialInfo& material)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end() || !it->second.data || materialIndex >= it->second.data->materials.size())
    {
        return;
    }
    // Editing a material is not evidence the model is being rendered, so this
    // deliberately does not bump lastAccess.
    ApplyImportedMaterialInfo(material, it->second.data->materials[materialIndex]);
}

void UpdateMaterials(const std::string& path, const std::vector<ModelImportedMaterialInfo>& materials)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    if (it == s_modelCache.end() || !it->second.data)
    {
        return;
    }
    const size_t count = std::min(materials.size(), it->second.data->materials.size());
    for (size_t i = 0; i < count; ++i)
    {
        ApplyImportedMaterialInfo(materials[i], it->second.data->materials[i]);
    }
}

void Invalidate(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::string dirPrefix = key;
    dirPrefix += static_cast<char>(std::filesystem::path::preferred_separator);

    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    for (auto it = s_modelCache.begin(); it != s_modelCache.end();)
    {
        if (it->first == key || it->first.starts_with(dirPrefix))
        {
            it = s_modelCache.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

size_t EstimateBytes(const LoadedModelData& data)
{
    size_t bytes = 0;
    for (const ModelSubmeshData& submesh : data.submeshes)
    {
        bytes += submesh.mesh.vertices.size() * sizeof(Vertex);
        bytes += submesh.mesh.indices.size() * sizeof(uint32_t);
    }
    return bytes;
}

size_t TotalBytes()
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    size_t total = 0;
    for (const auto& [key, entry] : s_modelCache)
    {
        total += entry.bytes;
    }
    return total;
}

void Trim(const std::unordered_set<std::string>& liveKeys, size_t budgetBytes)
{
    std::unordered_set<std::string> normalizedLive;
    normalizedLive.reserve(liveKeys.size());
    for (const std::string& liveKey : liveKeys)
    {
        normalizedLive.insert(NormalizeKey(liveKey));
    }

    std::lock_guard<std::mutex> lock(s_modelCacheMutex);

    size_t total = 0;
    std::vector<std::pair<uint64_t, std::string>> evictable;
    for (const auto& [key, entry] : s_modelCache)
    {
        total += entry.bytes;
        if (normalizedLive.count(key) == 0)
        {
            evictable.emplace_back(entry.lastAccess, key);
        }
    }

    if (total <= budgetBytes)
    {
        return;
    }

    // Oldest first.
    std::sort(evictable.begin(), evictable.end());
    for (const auto& [lastAccess, key] : evictable)
    {
        if (total <= budgetBytes)
        {
            break;
        }
        const auto it = s_modelCache.find(key);
        if (it == s_modelCache.end())
        {
            continue;
        }
        total -= it->second.bytes;
        s_modelCache.erase(it);
    }
}
}
}
