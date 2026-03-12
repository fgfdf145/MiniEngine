#pragma once

#include "model_loader.h"

#include <string>

class FbxModelLoader
{
public:
    static bool IsAvailable();
    static LoadedModelData LoadModel(const std::string& path);
};
