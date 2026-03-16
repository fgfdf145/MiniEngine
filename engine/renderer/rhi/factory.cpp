#include "factory.h"

#include "../renderer_shared_state.h"
#include <opengl/renderer.h>
#include <vulkan/renderer.h>

#include <stdexcept>

std::unique_ptr<IRenderBackend> CreateRenderBackend(
    RenderBackendType backendType,
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath
)
{
    switch (backendType)
    {
    case RenderBackendType::Vulkan:
        return std::make_unique<VulkanRenderer>(window, std::move(sharedState), std::move(startupModelPath));
    case RenderBackendType::OpenGL:
        return std::make_unique<OpenGLRenderer>(window, std::move(sharedState), std::move(startupModelPath));
    default:
        throw std::runtime_error("Unsupported render backend type");
    }
}
