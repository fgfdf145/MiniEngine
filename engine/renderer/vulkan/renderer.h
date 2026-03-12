#pragma once

#include "buffer.h"
#include "camera.h"
#include "command.h"
#include "device.h"
#include "imgui_layer.h"
#include "instance.h"
#include "model_loader.h"
#include "pipeline.h"
#include "render_pass.h"
#include "swapchain.h"
#include "texture.h"
#include "uniform_buffer.h"

#include <input/input.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Window;

struct RenderSubmesh
{
    std::unique_ptr<VulkanBuffer> buffer;
    uint32_t materialBindingIndex = 0;
    MaterialPushConstants material;
    std::string name;
};

struct MaterialTextureSlots
{
    uint32_t baseColor = 0;
    uint32_t normal = 0;
    uint32_t metallic = 0;
    uint32_t roughness = 0;
    uint32_t occlusion = 0;
    uint32_t emissive = 0;
};

class VulkanRenderer
{
public:
    explicit VulkanRenderer(Window& window, std::optional<std::string> startupModelPath = std::nullopt);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void HandleEvent(const SDL_Event& event);
    void DrawFrame();

private:
    void CreateSwapchainResources();
    void DestroySwapchainResources();
    void CreatePipelineResources();
    void DestroyPipelineResources();
    void RecreateSwapchain();
    bool HasDrawableArea() const;
    void LoadModel(const std::string& path);
    void ProcessPendingModelLoad();

    Window& m_window;
    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::vector<RenderSubmesh> m_renderSubmeshes;
    std::vector<std::unique_ptr<VulkanTexture>> m_textures;
    std::vector<MaterialTextureSlots> m_materialTextureSlots;
    std::unique_ptr<VulkanUniformBuffer> m_uniformBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<VulkanPipeline> m_graphicsPipeline;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
    InputState m_input;
    Camera m_camera;
    std::string m_currentModelPath;
    std::string m_lastModelLoadError;
    std::optional<std::string> m_pendingModelPath;
    std::chrono::steady_clock::time_point m_lastFrameTime = std::chrono::steady_clock::now();
};
