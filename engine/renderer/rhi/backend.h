#pragma once

#include <SDL3/SDL_events.h>

#include <optional>

enum class RenderBackendType
{
    Vulkan,
    OpenGL
};

inline const char* ToString(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return "Vulkan";
    case RenderBackendType::OpenGL:
        return "OpenGL";
    default:
        return "Unknown";
    }
}

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    virtual RenderBackendType GetBackendType() const = 0;
    virtual void HandleEvent(const SDL_Event& event) = 0;
    virtual void DrawFrame() = 0;
    virtual std::optional<RenderBackendType> ConsumeBackendSwitchRequest() = 0;
};
