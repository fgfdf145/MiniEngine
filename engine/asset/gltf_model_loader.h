#pragma once

#include "model_loader.h"

#include <string>

class GltfModelLoader
{
public:
    static LoadedModelData LoadModel(const std::string& path);
};
