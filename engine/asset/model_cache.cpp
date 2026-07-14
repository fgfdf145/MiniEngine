#include "model_cache.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace
{
std::mutex s_modelCacheMutex;
std::unordered_map<std::string, std::shared_ptr<LoadedModelData>> s_modelCache;
}

namespace ModelCache
{
bool IsCached(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    return s_modelCache.count(path) > 0;
}

std::shared_ptr<LoadedModelData> Get(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    const auto it = s_modelCache.find(path);
    return it != s_modelCache.end() ? it->second : nullptr;
}

void Store(const std::string& path, std::shared_ptr<LoadedModelData> data)
{
    std::lock_guard<std::mutex> lock(s_modelCacheMutex);
    s_modelCache[path] = std::move(data);
}
}
