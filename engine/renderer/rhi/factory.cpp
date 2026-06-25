#include "factory.h"

#include "../renderer_shared_state.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <window/window_platform.h>

#include <vulkan/renderer.h>

#include <stdexcept>

namespace
{
std::optional<std::string> ProbeVulkanRuntimeSupport()
{
    platform::window::ApplyPlatformWindowHints();

    const bool videoWasInitialized = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
    if (!videoWasInitialized)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            return std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
        }
    }

    SDL_ClearError();
    const bool loaded = SDL_Vulkan_LoadLibrary(nullptr);
    const char* errorText = SDL_GetError();
    const std::string error = (errorText != nullptr && errorText[0] != '\0')
        ? std::string(errorText)
        : std::string("SDL_Vulkan_LoadLibrary failed without an SDL error message");

    if (loaded)
    {
        SDL_Vulkan_UnloadLibrary();
    }

    if (!videoWasInitialized)
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    if (loaded)
    {
        return std::nullopt;
    }

    return error;
}

RenderBackendDescriptor DescribeVulkanBackend()
{
    return RenderBackendDescriptor{
        RenderBackendType::Vulkan,
        ToString(RenderBackendType::Vulkan),
        true,
        nullptr
    };
}
}

RenderBackendDescriptor GetRenderBackendDescriptor(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return DescribeVulkanBackend();
    default:
        return RenderBackendDescriptor{
            backendType,
            ToString(backendType),
            false,
            "Unsupported render backend"
        };
    }
}

bool IsRenderBackendSupported(RenderBackendType backendType)
{
    return GetRenderBackendDescriptor(backendType).isSupported;
}

std::optional<std::string> GetRenderBackendRuntimeError(RenderBackendType backendType)
{
    const RenderBackendDescriptor descriptor = GetRenderBackendDescriptor(backendType);
    if (!descriptor.isSupported)
    {
        return descriptor.unsupportedReason != nullptr
            ? std::optional<std::string>(descriptor.unsupportedReason)
            : std::optional<std::string>("Unsupported render backend");
    }

    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return ProbeVulkanRuntimeSupport();
    default:
        return std::nullopt;
    }
}

RenderBackendType GetPreferredRenderBackendType()
{
    if (IsRenderBackendSupported(RenderBackendType::Vulkan) &&
        !GetRenderBackendRuntimeError(RenderBackendType::Vulkan).has_value())
    {
        return RenderBackendType::Vulkan;
    }

    throw std::runtime_error("Vulkan backend is not available at runtime");
}

std::unique_ptr<IRenderBackend> CreateRenderBackend(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    RenderBackendType backendType,
    std::optional<std::string> startupModelPath
)
{
    if (const std::optional<std::string> runtimeError = GetRenderBackendRuntimeError(backendType); runtimeError.has_value())
    {
        throw std::runtime_error(*runtimeError);
    }

    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return std::make_unique<VulkanRenderer>(window, std::move(sharedState), std::move(startupModelPath));
    default:
        throw std::runtime_error("Unsupported render backend");
    }
}
