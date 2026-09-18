#pragma once

#include "buffer.h"
#include "command.h"
#include "device.h"
#include "exposure_histogram_pass.h"
#include "forward_pass.h"
#include "imgui_layer.h"
#include "instance.h"
#include "pipeline_set.h"
#include "render_pass.h"
#include "scene_render_targets.h"
#include "swapchain.h"
#include "texture.h"
#include "tonemap_pass.h"
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
    void SyncSceneTargets();
    void UploadSceneResources();
    void ApplyRenderContent(
        std::vector<std::unique_ptr<VulkanTexture>> newTextures,
        std::vector<MaterialTextureSlots> newMaterialTextureSlots,
        std::vector<RenderSubmesh> newRenderSubmeshes);
    std::vector<VulkanDrawItem> BuildDrawItems(uint32_t imageIndex) const;
    void RecordTransitions(
        VkCommandBuffer commandBuffer,
        const RenderPassIo& io,
        const ScenePassFrameContext& frame);
    void RecordScenePasses(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame);
    void RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex) const;
    // Meters the histogram the given frame slot last wrote and moves the camera's EV100 toward
    // it. Must run after AcquireNextImage has waited on that slot's fence.
    void UpdateAutoExposure(uint32_t frameSlot);

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
    // Device-lifetime resources: the shader-fixed frame and material set layouts and the pipeline
    // cache all outlive every swapchain, viewport and scene reload (see CreateDeviceResources).
    std::unique_ptr<VulkanFrameDescriptorSetLayout> m_frameSetLayout;
    std::unique_ptr<VulkanMaterialDescriptorSetLayout> m_materialSetLayout;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    std::unique_ptr<VulkanUniformBuffer> m_uniformBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<SceneRenderTargets> m_sceneTargets;
    std::unique_ptr<VulkanForwardPass> m_forwardPass;
    std::unique_ptr<VulkanExposureHistogramPass> m_exposurePass;
    // False until auto exposure has metered its first frame, which it then snaps to instead of
    // fading in from the default EV.
    bool m_hasMeteredExposure = false;
    std::unique_ptr<VulkanTonemapPass> m_tonemapPass;
    // Scoped to one command buffer: the recording lambda resets it per frame, because a target's
    // layout belongs to one of its per-frame copies and not to the target as a whole. The resets
    // at the image lifetime boundaries keep it from describing a destroyed image even when no
    // frame is recorded in between.
    RenderTargetLayoutTracker m_layoutTracker;
    std::vector<IScenePass*> m_scenePasses;
    std::unique_ptr<VulkanPipelineSet> m_graphicsPipelines;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
};
}
