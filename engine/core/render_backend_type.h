#pragma once

#include <string_view>

enum class RenderBackendType
{
    Vulkan
};

inline const char* ToString(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return "Vulkan";
    default:
        return "Unknown";
    }
}

inline RenderBackendType GetDefaultRenderBackendType()
{
    return RenderBackendType::Vulkan;
}

inline bool UsesZeroToOneDepth(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return true;
    default:
        return false;
    }
}

inline bool UsesInvertedRenderYAxis(RenderBackendType backendType)
{
    return backendType == RenderBackendType::Vulkan;
}

inline bool TryParseRenderBackendType(std::string_view value, RenderBackendType& backendType)
{
    if (value == "vulkan")
    {
        backendType = RenderBackendType::Vulkan;
        return true;
    }

    return false;
}
