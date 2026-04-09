#include "factory.h"

#include "../renderer_shared_state.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <window/window_platform.h>

#include <array>
#if defined(MINIENGINE_HAS_VULKAN_BACKEND)
#include <vulkan/renderer.h>
#endif

#include <stdexcept>

#if defined(__APPLE__)
#include <metal/renderer.h>
#endif

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
#if defined(MINIENGINE_HAS_VULKAN_BACKEND)
    return RenderBackendDescriptor{
        RenderBackendType::Vulkan,
        ToString(RenderBackendType::Vulkan),
        true,
        nullptr
    };
#else
    return RenderBackendDescriptor{
        RenderBackendType::Vulkan,
        ToString(RenderBackendType::Vulkan),
        false,
        "Vulkan backend is not built in this configuration"
    };
#endif
}

RenderBackendDescriptor DescribeMetalBackend()
{
#if defined(__APPLE__)
    return RenderBackendDescriptor{
        RenderBackendType::Metal,
        ToString(RenderBackendType::Metal),
        true,
        nullptr
    };
#else
    return RenderBackendDescriptor{
        RenderBackendType::Metal,
        ToString(RenderBackendType::Metal),
        false,
        "Metal backend is only available on Apple platforms"
    };
#endif
}
}

RenderBackendDescriptor GetRenderBackendDescriptor(RenderBackendType backendType)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return DescribeVulkanBackend();
    case RenderBackendType::Metal:
        return DescribeMetalBackend();
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
    case RenderBackendType::Metal:
    default:
        return std::nullopt;
    }
}

RenderBackendType GetPreferredRenderBackendType()
{
#if defined(__APPLE__)
    if (IsRenderBackendSupported(RenderBackendType::Vulkan) &&
        !GetRenderBackendRuntimeError(RenderBackendType::Vulkan).has_value())
    {
        return RenderBackendType::Vulkan;
    }

    if (IsRenderBackendSupported(RenderBackendType::Metal))
    {
        return RenderBackendType::Metal;
    }
#else
    if (IsRenderBackendSupported(RenderBackendType::Vulkan))
    {
        return RenderBackendType::Vulkan;
    }
#endif

    constexpr std::array<RenderBackendType, 2> kFallbackOrder = {
        RenderBackendType::Vulkan,
        RenderBackendType::Metal
    };
    for (RenderBackendType backendType : kFallbackOrder)
    {
        if (IsRenderBackendSupported(backendType) &&
            !GetRenderBackendRuntimeError(backendType).has_value())
        {
            return backendType;
        }
    }

    throw std::runtime_error("No supported render backend is available in this build");
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
#if defined(MINIENGINE_HAS_VULKAN_BACKEND)
        return std::make_unique<VulkanRenderer>(window, std::move(sharedState), std::move(startupModelPath));
#else
        throw std::runtime_error("Vulkan backend is not built in this configuration");
#endif
    case RenderBackendType::Metal:
#if defined(__APPLE__)
        return std::make_unique<MetalRenderer>(window, std::move(sharedState), std::move(startupModelPath));
#else
        throw std::runtime_error("Metal backend is only available on Apple platforms");
#endif
    default:
        throw std::runtime_error("Unsupported render backend");
    }
}
