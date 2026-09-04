#pragma once

#include <engine/core/render_backend_type.h>
#include <SDL3/SDL_events.h>

namespace me
{

struct RenderBackendDescriptor
{
    RenderBackendType type = RenderBackendType::Vulkan;
    const char* name = "Unknown";
    bool isSupported = false;
    const char* unsupportedReason = nullptr;
};

class IRenderBackend
{
  public:
    virtual ~IRenderBackend() = default;

    virtual RenderBackendType GetBackendType() const = 0;
    virtual void HandleEvent(const SDL_Event& event) = 0;
    virtual void DrawFrame() = 0;
};
}
