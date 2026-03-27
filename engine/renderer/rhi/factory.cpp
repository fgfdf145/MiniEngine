#include "factory.h"

#include "../renderer_shared_state.h"
#include <vulkan/renderer.h>

std::unique_ptr<IRenderBackend> CreateRenderBackend(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath
)
{
    return std::make_unique<VulkanRenderer>(window, std::move(sharedState), std::move(startupModelPath));
}
