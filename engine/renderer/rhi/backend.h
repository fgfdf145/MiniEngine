#pragma once

#include <engine/core/render_backend_type.h>
#include <SDL3/SDL_events.h>

#include <cstdint>
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
    // Compares the DDGI probes' irradiance on every stride-th pixel's surface (Graphics Debug view 15,
    // which the last frame must have shown) with a CPU path tracer's over the same ray scene, and
    // writes prefix_ddgi.pfm, prefix_reference.pfm and prefix_compare.png. For verification runs
    // (--reference).
    virtual void CaptureDdgiReference(const std::filesystem::path& prefix, uint32_t samples, uint32_t stride)
    {
        (void)prefix;
        (void)samples;
        (void)stride;
        throw std::runtime_error("This render backend cannot compare DDGI with a reference");
    }
};
}
