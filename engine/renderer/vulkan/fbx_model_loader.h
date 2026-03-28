#pragma once

#include "model_loader.h"

#include <string>

class FbxModelLoader
{
public:
    static LoadedModelData LoadModel(const std::string& path);
};
