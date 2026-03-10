#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "render_pass.h"
#include "swapchain.h"

#include <memory>

class Window;

class VulkanRenderer
{
public:
    explicit VulkanRenderer(Window& window);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void DrawFrame();

private:
    void LoadVulkanLoader();
    void UnloadVulkanLoader();
    void CreateSwapchainResources();
    void DestroySwapchainResources();
    void RecreateSwapchain();
    bool HasDrawableArea() const;

    Window& m_window;
    bool m_vulkanLoaderLoaded = false;
    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::unique_ptr<VulkanBuffer> m_vertexBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<VulkanPipeline> m_graphicsPipeline;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
};
