#pragma once

#include <string_view>

enum class RenderBackendType
{
    Vulkan,
    Metal
};

inline const char* ToString(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return "Vulkan";
    case RenderBackendType::Metal:
        return "Metal";
    default:
        return "Unknown";
    }
}

inline RenderBackendType GetDefaultRenderBackendType()
{
#if defined(__APPLE__)
    return RenderBackendType::Metal;
#else
    return RenderBackendType::Vulkan;
#endif
}

inline bool UsesZeroToOneDepth(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
    case RenderBackendType::Metal:
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

    if (value == "metal")
    {
        backendType = RenderBackendType::Metal;
        return true;
    }

    return false;
}
