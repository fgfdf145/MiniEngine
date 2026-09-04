#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "imgui_layer.h"
#include "instance.h"
#include "pipeline_set.h"
#include "render_pass.h"
#include "scene_viewport.h"
#include "swapchain.h"
#include "texture.h"
#include "uniform_buffer.h"

#include <engine/editor/editor_backend_base.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

class Window;

struct RenderSubmesh
{
    entt::entity entity = entt::null;
    std::unique_ptr<VulkanBuffer> buffer;
    uint32_t materialBindingIndex = 0;
    MaterialPushConstants material;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    glm::vec3 localBoundsCenter{0.0f};
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
    uint32_t secondaryBaseColor = 0;
    uint32_t secondaryNormal = 0;
    uint32_t secondaryMetallic = 0;
    uint32_t secondaryRoughness = 0;
    uint32_t secondaryOcclusion = 0;
    uint32_t secondaryEmissive = 0;
    uint32_t blendMask = 0;
};

class VulkanRenderer : public EditorRenderBackendBase
{
  public:
    VulkanRenderer(
        Window& window,
        std::shared_ptr<RendererSharedState> sharedState,
        std::optional<std::string> startupModelPath = std::nullopt);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void DrawFrame() override;

  protected:
    void HandleBackendEvent(const SDL_Event& event) override;
    bool WantsKeyboardCapture() const override;

  private:
    void CreateDeviceResources();
    void DestroyDeviceResources();
    void CreateSwapchainResources();
    void DestroySwapchainResources();
    void CreateDescriptorResources();
    void DestroyDescriptorResources();
    void EnsureGraphicsPipelines();
    void RecreateSwapchain();
    void SyncSceneViewportLayer();
    void UploadSceneResources();
    void ApplyRenderContent(
        std::vector<std::unique_ptr<VulkanTexture>> newTextures,
        std::vector<MaterialTextureSlots> newMaterialTextureSlots,
        std::vector<RenderSubmesh> newRenderSubmeshes);
    std::vector<VulkanDrawItem> BuildDrawItems(uint32_t imageIndex) const;
    void RecordSceneLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex, const std::vector<VulkanDrawItem>& drawItems) const;
    void RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex) const;

    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::vector<RenderSubmesh> m_renderSubmeshes;
    std::vector<std::unique_ptr<VulkanTexture>> m_textures;
    // Parallel to m_textures: the cache key ("path|srgb" or "__id__|linear") for each slot.
    // Used to move live textures into the pool before a rebuild so they can be reused without
    // re-uploading them to the GPU.
    std::vector<std::string> m_textureCacheKeys;
    std::unordered_map<std::string, std::unique_ptr<VulkanTexture>> m_texturePool;
    std::vector<MaterialTextureSlots> m_materialTextureSlots;
    // Device-lifetime resources: the shader-fixed material set layout and the pipeline cache both
    // outlive every swapchain, viewport and scene reload (see CreateDeviceResources).
    std::unique_ptr<VulkanMaterialDescriptorSetLayout> m_materialSetLayout;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    std::unique_ptr<VulkanUniformBuffer> m_uniformBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<VulkanSceneViewport> m_sceneViewportLayer;
    std::unique_ptr<VulkanPipelineSet> m_graphicsPipelines;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
};
}
