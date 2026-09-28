#pragma once

#include <string_view>

namespace me
{

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

// The render projection is reverse-Z (near plane at depth 1, far plane at 0; see
// engine/renderer/reverse_depth.h). The editor's own projection, which ImGuizmo and picking use,
// stays conventional.
inline bool UsesReverseRenderDepth(RenderBackendType backendType)
{
    return backendType == RenderBackendType::Vulkan;
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
}
