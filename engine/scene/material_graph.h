#pragma once

#include <string>

struct MaterialTextureBlendGraph
{
    bool enabled = false;
    float blendFactor = 0.0f;
    std::string blendMaskTexturePath;
    std::string secondaryBaseColorTexturePath;
    std::string secondaryNormalTexturePath;
    std::string secondaryMetallicTexturePath;
    std::string secondaryRoughnessTexturePath;
    std::string secondaryOcclusionTexturePath;
    std::string secondaryEmissiveTexturePath;
};
