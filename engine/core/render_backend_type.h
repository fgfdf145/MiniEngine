#pragma once

#include <string_view>

namespace me
{

enum class RenderBackendType
{
    Vulkan,
    // Direct3D 12 (Windows): the same renderer on NVRHI's D3D12 backend
    // (docs/design/2026-10-09-d3d12-backend-design.md).
    D3D12
};

inline const char* ToString(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return "Vulkan";
    case RenderBackendType::D3D12:
        return "Direct3D 12";
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
    case RenderBackendType::D3D12:
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
    return backendType == RenderBackendType::Vulkan || backendType == RenderBackendType::D3D12;
}

// Both backends run the same shaders, written for Vulkan's clip space (the D3D12 build mirrors y as
// the vertex shaders write it, ClipPosition), so both take the same projection.
inline bool UsesInvertedRenderYAxis(RenderBackendType backendType)
{
    return backendType == RenderBackendType::Vulkan || backendType == RenderBackendType::D3D12;
}

inline bool TryParseRenderBackendType(std::string_view value, RenderBackendType& backendType)
{
    if (value == "vulkan")
    {
        backendType = RenderBackendType::Vulkan;
        return true;
    }
    if (value == "d3d12" || value == "dx12")
    {
        backendType = RenderBackendType::D3D12;
        return true;
    }

    return false;
}
}
