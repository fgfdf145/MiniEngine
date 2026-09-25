#pragma once

#include "ao_pass.h"
#include "bloom_pass.h"
#include "atmosphere.h"
#include "environment_probe.h"
#include "buffer.h"
#include "command.h"
#include "device.h"
#include "exposure_histogram_pass.h"
#include "forward_pass.h"
#include "gbuffer_inputs.h"
#include "geometry_pass.h"
#include "imgui_layer.h"
#include "lighting_pass.h"
#include "local_shadow_pass.h"
#include "instance.h"
#include "pipeline_set.h"
#include "render_pass.h"
#include "scene_render_targets.h"
#include "shadow_pass.h"
#include "swapchain.h"
#include "ssr_pass.h"
#include "taa_pass.h"
#include "texture.h"
#include "tonemap_pass.h"
#include "uniform_buffer.h"

#include <engine/editor/editor_backend_base.h>
#include <engine/asset/texture_preparation.h>
#include <engine/renderer/taa_jitter.h>
#include <engine/renderer/temporal_history.h>
#include <engine/renderer/motion_history.h>

#include <memory>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me
{

class Window;

struct RenderSubmesh
{
    entt::entity entity = entt::null;
    std::unique_ptr<VulkanBuffer> buffer;
    uint32_t materialBindingIndex = 0;
    GpuMaterialData material;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    glm::vec3 localBoundsCenter{0.0f};
    float localBoundsRadius = 0.0f;
    std::string name;
    // Assigned in ApplyRenderContent: the entity and this submesh's position among that entity's
    // submeshes, which is what MotionHistory finds last frame's model matrix by.
    MotionKey motionKey;
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
    uint32_t clearcoat = 0;
    uint32_t clearcoatRoughness = 0;
    uint32_t sheenColor = 0;
    uint32_t sheenRoughness = 0;
    uint32_t anisotropy = 0;
    uint32_t specular = 0;
    uint32_t specularColor = 0;
    uint32_t clearcoatNormal = 0;
};

// One texture of a content upload in progress: either created by that upload, or a live texture
// it reuses, named by its index in VulkanRenderer::m_textures. Reuse is by index rather than by
// taking ownership, so the live list stays intact until the whole upload has succeeded.
struct PendingTexture
{
    static constexpr size_t kNotReused = static_cast<size_t>(-1);

    std::unique_ptr<VulkanTexture> created;
    size_t reusedIndex = kNotReused;
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
    void CaptureViewport(const std::filesystem::path& path) override;

  protected:
    void HandleBackendEvent(const SDL_Event& event) override;
    bool WantsKeyboardCapture() const override;

  private:
    void CreateDeviceResources();
    void DestroyDeviceResources();
    EnvironmentDescriptorBindings BuildEnvironmentBindings() const;
    EnvironmentMode EffectiveEnvironmentMode(const SceneEnvironment& environment) const;
    // Starts, finishes or skips the background decode of the scene's HDRI; installs it when ready.
    void UpdateEnvironmentMap(const SceneEnvironment& environment);
    void CreateSwapchainResources();
    void CreateScenePasses();
    IScenePass* FindScenePass(ScenePassId id) const;
    void DestroySwapchainResources();
    void CreateDescriptorResources();
    void DestroyDescriptorResources();
    void RecreateSwapchain();
    bool SwapchainNeedsResize() const;
    void SyncSceneTargets();
    // Builds the GPU content for the scene as it now is and swaps it in. Transactional: when it
    // throws, the previous content, textures and descriptor sets are untouched and still drawable.
    void UploadSceneResources();
    // UploadSceneResources for a change made while the editor runs. Running out of GPU memory is
    // reported in the editor and leaves the previous content on screen instead of ending the
    // program; any other failure still propagates.
    void UploadSceneResourcesOrKeepPrevious();
    // A renderables change: queues the texture files it needs that are not already on the GPU, and
    // marks an upload pending. The upload itself happens in PumpSceneUpload.
    void RequestSceneUpload();
    // Called every frame: uploads a few textures the workers finished into the staged set and, once
    // a pending change has every texture it needs, commits it with UploadSceneResourcesOrKeepPrevious.
    // Also keeps State().sceneUploadStatus current.
    void PumpSceneUpload();
    // Forgets a pending change's staged textures and failures, after it ran out of memory.
    void AbandonPendingTextures();
    std::unique_ptr<VulkanTexture> UploadPreparedTexture(
        const PreparedTexture& prepared,
        TextureUsage usage,
        VulkanUploadBatch& uploadBatch);
    // After a failed upload the previous content may still name entities the change deleted.
    // Drawing one would read a destroyed entity's transform, so those submeshes are dropped.
    void DropSubmeshesOfRemovedEntities();
    void ApplyRenderContent(
        std::vector<PendingTexture> newTextures,
        std::vector<std::string> newTextureCacheKeys,
        std::vector<MaterialTextureSlots> newMaterialTextureSlots,
        std::vector<RenderSubmesh> newRenderSubmeshes);
    // models is parallel to m_renderSubmeshes: this frame's model matrix of each submesh.
    std::vector<VulkanDrawItem> BuildDrawItems(uint32_t imageIndex, std::span<const glm::mat4> models) const;
    std::vector<ShadowDrawItem> BuildShadowDrawItems(uint32_t imageIndex) const;
    void RecordTransitions(
        VkCommandBuffer commandBuffer,
        const RenderPassIo& io,
        const ScenePassFrameContext& frame);
    void RecordScenePasses(
        VkCommandBuffer commandBuffer,
        const ScenePassFrameContext& frame,
        std::span<const ScenePassId> passOrder);
    void RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex) const;
    // Meters the histogram the given frame slot last wrote and moves the camera's EV100 toward
    // it. Must run after AcquireNextImage has waited on that slot's fence.
    void UpdateAutoExposure(uint32_t frameSlot);
    // Adapts the white point toward this frame's illuminant estimate and returns the balance the
    // tone mapping pass applies (identity when auto white balance is off).
    glm::mat3 UpdateWhiteBalance();
    // Logs when the number of lights left out by the light limit changes.
    void ReportDroppedLights(uint32_t droppedCount);
    void ReportDroppedClusterLights(uint32_t droppedCount);
    void ReportDroppedLocalShadows(uint32_t droppedCount);

    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::vector<RenderSubmesh> m_renderSubmeshes;
    std::vector<std::unique_ptr<VulkanTexture>> m_textures;
    // Parallel to m_textures: the cache key ("path|srgb" or "__id__|linear") for each slot. A
    // rebuild looks live textures up by it and reuses them instead of uploading them again.
    std::vector<std::string> m_textureCacheKeys;
    // Prepares texture files on worker threads; see RequestSceneUpload and PumpSceneUpload.
    std::unique_ptr<TexturePreparationQueue> m_texturePreparation;
    // Textures prepared and uploaded for a change that has not committed yet, by cache key. The
    // commit moves the ones it uses into m_textures and releases the rest.
    std::unordered_map<std::string, std::unique_ptr<VulkanTexture>> m_stagedTextures;
    // Keys the workers could not decode; their slots use the default texture.
    std::unordered_set<std::string> m_failedTextureKeys;
    bool m_sceneUploadPending = false;
    // Texture files requested since the last commit, for the progress status.
    size_t m_texturesRequested = 0;
    struct TextureUploadStats
    {
        size_t fromCache = 0;
        size_t compressedNow = 0;
        size_t uncompressed = 0;
        size_t floatTextures = 0;
        double compressSeconds = 0.0;
    };
    // Counted as textures upload, logged and reset when a change commits.
    TextureUploadStats m_textureUploadStats;
    std::vector<MaterialTextureSlots> m_materialTextureSlots;
    // Device-lifetime resources: the shader-fixed frame and material set layouts and the pipeline
    // cache all outlive every swapchain, viewport and scene reload (see CreateDeviceResources).
    std::unique_ptr<VulkanFrameDescriptorSetLayout> m_frameSetLayout;
    std::unique_ptr<VulkanMaterialDescriptorSetLayout> m_materialSetLayout;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    // Device lifetime too: its image has a fixed size and is shared by every frame in flight, and
    // every VulkanUniformBuffer binds it into set 0.
    std::unique_ptr<VulkanShadowPass> m_shadowPass;
    std::unique_ptr<VulkanLocalShadowPass> m_localShadowPass;
    // Device lifetime as well, for the same reasons: fixed-size images shared by every frame in
    // flight and bound into set 0 by every VulkanUniformBuffer.
    std::unique_ptr<VulkanAtmosphere> m_atmosphere;
    // The sky prefiltered for the specular lobe, and the DFG table it is weighted by.
    std::unique_ptr<VulkanEnvironmentProbe> m_environmentProbe;
    std::unique_ptr<VulkanTexture> m_environmentBrdfLut;
    // The area lights' LTC tables (ltc_table.h), one mip each, RGBA32F.
    std::unique_ptr<VulkanTexture> m_ltcInverseMatrices;
    std::unique_ptr<VulkanTexture> m_ltcAmplitudes;
    // Set 0 binding 6 when no HDRI is loaded.
    std::unique_ptr<VulkanTexture> m_defaultEnvironmentMap;
    // The loaded HDRI and the scene path it came from; empty until one loads.
    std::unique_ptr<VulkanTexture> m_environmentMap;
    std::string m_environmentMapPath;
    // A decode running on a worker thread, and the path it decodes.
    // A decoded HDRI and its SH, prepared together on the worker thread.
    struct PreparedEnvironmentMap
    {
        FloatTextureData image;
        ShCoefficients sh{};
    };
    std::future<PreparedEnvironmentMap> m_pendingEnvironmentMap;
    // The loaded HDRI's unrotated radiance SH, at intensity 1.
    ShCoefficients m_environmentMapSh{};
    std::string m_pendingEnvironmentMapPath;
    // The last path that failed, so a bad file is reported once rather than every frame.
    std::string m_failedEnvironmentMapPath;
    // The swapchain image the last submitted frame drew into, for CaptureViewport.
    std::optional<uint32_t> m_lastRecordedImageIndex;
    std::unique_ptr<VulkanUniformBuffer> m_uniformBuffer;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    std::unique_ptr<SceneRenderTargets> m_sceneTargets;
    // False until auto exposure has metered its first frame, which it then snaps to instead of
    // fading in from the default EV.
    bool m_hasMeteredExposure = false;
    // Two-stage auto exposure (see StepAutoExposure): the long-term stage, and the sun and sky
    // references gathered while recording the previous frame.
    AutoExposureState m_autoExposureState;
    ExposureReferences m_exposureReferences;
    // Auto white balance (see white_balance.h): the references gathered with the exposure ones, and
    // the adapted white point; empty until the first balanced frame.
    WhiteBalanceReferences m_whiteBalanceReferences;
    std::optional<glm::vec2> m_adaptedWhiteXy;
    // The HDR output setting the current swapchain was created for; a different one recreates it.
    bool m_swapchainHdrRequested = false;
    uint32_t m_droppedLightCount = 0;
    uint32_t m_droppedClusterLightCount = 0;
    uint32_t m_droppedLocalShadowCount = 0;
    // Scoped to one command buffer: the recording lambda resets it per frame, because a target's
    // layout belongs to one of its per-frame copies and not to the target as a whole. The resets
    // at the image lifetime boundaries keep it from describing a destroyed image even when no
    // frame is recorded in between.
    RenderTargetLayoutTracker m_layoutTracker;
    // Last frame's matrices for motion vectors. Reset wherever the scene targets are rebuilt,
    // because a new extent is a new projection and would otherwise read as full-screen motion.
    MotionHistory m_motionHistory;
    // Which AO history image each frame reads and writes. Reset with the motion history, because
    // the resolve pass recreates its history images at the same points.
    TemporalHistory m_aoHistory;
    TemporalHistory m_ssrHistory;
    TemporalHistory m_taaHistory;
    // The pre-exposure the TAA history was written with; 0 before any frame wrote it.
    float m_taaHistoryPreExposure = 0.0f;
    // Advances once per frame that jitters; picks the frame's offset in the TAA jitter sequence.
    uint32_t m_taaFrameIndex = 0;
    // Seeds the AO trace's noise; advances once per recorded frame.
    uint32_t m_aoFrameIndex = 0;
    // Set 2 and the set 1 filler for every pass that samples the G-buffer. Rebuilt with the passes
    // on a swapchain recreate, and rewritten before them on a viewport resize.
    std::unique_ptr<VulkanGBufferDescriptors> m_gbufferDescriptors;
    // Every scene pass, owned, in construction order. Record order is decided per frame by
    // BuildScenePassOrder and resolved through FindScenePass, so this list is only ever walked
    // whole — when the targets are rebuilt. Adding a pass is one push_back in CreateScenePasses.
    std::vector<std::unique_ptr<IScenePass>> m_scenePasses;
    // Non-owning: the exposure pass inside m_scenePasses, kept typed because UpdateAutoExposure
    // reads its histograms. Set and cleared together with the list, so it is null exactly when
    // the list is empty, which UpdateAutoExposure already checks for.
    VulkanExposureHistogramPass* m_exposurePass = nullptr;
    std::unique_ptr<VulkanPipelineSet> m_forwardPipelines;
    std::unique_ptr<VulkanPipelineSet> m_geometryPipelines;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
};
}
