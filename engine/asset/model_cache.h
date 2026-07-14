#pragma once

#include "model_loader.h"

#include <memory>
#include <string>

// Thread-safe cache of parsed model data, keyed by source path.
// Written by background loader threads; read by the main thread.
namespace ModelCache
{
bool IsCached(const std::string& path);
std::shared_ptr<LoadedModelData> Get(const std::string& path);
void Store(const std::string& path, std::shared_ptr<LoadedModelData> data);
}
