#pragma once

#include <engine/core/render_backend_type.h>
#include <SDL3/SDL_events.h>

#include <filesystem>
#include <stdexcept>

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
    // Writes the viewport of the last drawn frame to a PNG. For verification runs (--capture).
    virtual void CaptureViewport(const std::filesystem::path& path)
    {
        (void)path;
        throw std::runtime_error("This render backend cannot capture the viewport");
    }
};
}
