#pragma once

#include "buffer.h"
#include "camera.h"
#include "command.h"
#include "device.h"
#include "imgui_layer.h"
#include "instance.h"
#include "pipeline.h"
#include "render_pass.h"
#include "swapchain.h"
#include "uniform_buffer.h"

#include <input/input.h>

#include <chrono>
#include <memory>

class Window;

class VulkanRenderer
{
public:
    explicit VulkanRenderer(Window& window);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void HandleEvent(const SDL_Event& event);
    void DrawFrame();

private:
    void CreateSwapchainResources();
    void DestroySwapchainResources();
    void RecreateSwapchain();
    bool HasDrawableArea() const;

    Window& m_window;
    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::unique_ptr<VulkanBuffer> m_vertexBuffer;
    std::unique_ptr<VulkanUniformBuffer> m_uniformBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<VulkanPipeline> m_graphicsPipeline;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
    InputState m_input;
    Camera m_camera;
    std::chrono::steady_clock::time_point m_lastFrameTime = std::chrono::steady_clock::now();
};
