#include "model_cache.h"

#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace
{
std::mutex s_modelCacheMutex;
std::unordered_map<std::string, std::shared_ptr<LoadedModelData>> s_modelCache;

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
    return s_modelCache.count(key) > 0;
}

std::shared_ptr<LoadedModelData> Get(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(key);
    return it != s_modelCache.end() ? it->second : nullptr;
}

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data)
{
    const std::string key = NormalizeKey(path);
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    s_modelCache[key] = std::move(data);
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
}
