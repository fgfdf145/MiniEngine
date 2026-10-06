#pragma once

#include "ao_pass.h"
#include "bloom_pass.h"
#include "atmosphere.h"
#include "environment_probe.h"
#include "buffer.h"
#include "command.h"
#include "device.h"
#include "dlss.h"
#include "exposure_histogram_pass.h"
#include "forward_pass.h"
#include "gbuffer_inputs.h"
#include "sampler_settings.h"
#include "scatter_pass.h"
#include "transmission_copy.h"
#include "geometry_pass.h"
#include "imgui_layer.h"
#include "lighting_pass.h"
#include "local_shadow_pass.h"
#include "instance.h"
#include "pipeline_set.h"
#include "render_frame_packet.h"
#include "render_pass.h"
#include "gi_pass.h"
#include "gpu_timer.h"
#include "ddgi_debug_pass.h"
#include "ray_scene.h"
#include "rt_shadow_pass.h"
#include "ddgi.h"
#include "scene_render_targets.h"
#include "selection_outline_pass.h"
#include "shadow_pass.h"
#include "swapchain.h"
#include "ssr_pass.h"
#include "taa_pass.h"
#include "texture.h"
#include "tonemap_pass.h"
#include "material_set_cache.h"
#include "parallel_recorder.h"
#include "uniform_buffer.h"
#include "video_readback.h"

#include <engine/editor/editor_backend_base.h>
#include <engine/renderer/volumetric_clouds.h>
#include <engine/asset/texture_preparation.h>
#include <engine/core/threading/render_thread.h>
#include <engine/renderer/cpu_stage_timer.h>
#include <engine/renderer/taa_jitter.h>
#include <engine/renderer/view_frustum.h>
#include <engine/renderer/temporal_history.h>
#include <engine/renderer/motion_history.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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
struct ImageCaptureRequest;

// A CPU submesh as the GPU draws it. Made once per CpuRenderSubmesh revision and shared from one content
// upload to the next while that revision is still in the scene, so a change costs what changed.
struct RenderSubmesh : std::enable_shared_from_this<RenderSubmesh>
{
    entt::entity entity = entt::null;
    // The CpuRenderSubmesh revision this was made from.
    uint64_t revision = 0;
    // The CPU geometry, shared with the model cache: the ray scene builds its hierarchy from it.
    std::shared_ptr<const MeshData> mesh;
    // Shared: an upload keeps the buffers of meshes that were already on the GPU (UploadSceneResources),
    // so adding or removing one model leaves every other model's geometry where it is.
    std::shared_ptr<VulkanBuffer> buffer;
    // Set 1, from VulkanMaterialSetCache, and the textures it names by cache key (built-in defaults
    // left out): an upload that keeps this submesh keeps those textures.
    VkDescriptorSet materialSet = VK_NULL_HANDLE;
    std::vector<std::string> textureKeys;
    // The base colour and emission the ray scene averages for this submesh's ray material, and the
    // metallic and roughness maps its hit shading samples with them.
    TextureDescriptorBinding rayBaseColor;
    TextureDescriptorBinding rayEmissive;
    TextureDescriptorBinding rayMetallic;
    TextureDescriptorBinding rayRoughness;
    // The draw slot (firstInstance) holding this submesh's material, texture transforms, previous model
    // matrix and ray material, from the commit that first draws it until the one that drops it. Set at
    // commit, hence mutable: everything else is fixed once made.
    static constexpr uint32_t kNoDrawSlot = UINT32_MAX;
    mutable uint32_t drawSlot = kNoDrawSlot;
    GpuMaterialData material;
    GpuTextureTransforms textureTransforms;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool decal = false;
    glm::vec3 localBoundsCenter{0.0f};
    float localBoundsRadius = 0.0f;
    std::string name;
    // The entity and this submesh's position among that entity's submeshes, which is what
    // MotionHistory finds last frame's model matrix by. A revision keeps its position: replacing an
    // entity's submeshes gives them all new revisions.
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
    uint32_t iridescence = 0;
    uint32_t iridescenceThickness = 0;
    uint32_t transmission = 0;
    uint32_t thickness = 0;
    uint32_t diffuseTransmission = 0;
    uint32_t diffuseTransmissionColor = 0;
    uint32_t detailMask = 0;
    std::array<uint32_t, kDetailLayerCount> detailLayers{};
    // The sampler each binding pairs its texture with, in binding order (MaterialTextureSlot); the
    // blend graph's slots keep the default.
    MaterialTextureSamplers samplers{};
};

// A texture on the GPU, by cache key, and how many live render submeshes name it. The built-in
// defaults are permanent; any other texture goes at the first commit that leaves it unreferenced.
struct StoredTexture
{
    std::unique_ptr<VulkanTexture> texture;
    uint32_t references = 0;
    bool permanent = false;
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

    // On the main thread: runs the frame's simulation and editor UI, then hands what it draws to the
    // render thread (RenderFrame), which draws it while the next frame runs here.
    void DrawFrame() override;
    // These wait for the render thread to finish the frames handed to it, then work alone.
    void CaptureViewport(const std::filesystem::path& path) override;
    void CaptureDdgiReference(const DdgiReferenceRequest& reference) override;
    void LogFrameTimings() const override;

  protected:
    void HandleBackendEvent(const SDL_Event& event) override;
    bool WantsKeyboardCapture() const override;
    void FlushVideoFrames() override;
    void RunWithRenderIdle(const std::function<void()>& work) override;

  private:
    // Main thread: takes what the render thread reported of its last frame (exposure, upload status).
    void ApplyRenderFeedback();
    // Main thread, after ImGui::Render: makes, updates or destroys the textures ImGui asked for.
    void ApplyImGuiTextureRequests(const ImDrawData& drawData);
    // Main thread: everything the render thread will read of this frame.
    void BuildFramePacket(RenderFramePacket& packet, bool contentChanged, RenderExtent viewportExtent);
    // Render thread: draws one frame from its packet.
    void RenderFrame(RenderFramePacket& frame);
    void PublishFeedback(const RenderFramePacket& frame);
    void CaptureViewportNow(const std::filesystem::path& path);
    // In ddgi_reference_capture.cpp.
    void CaptureDdgiReferenceNow(const DdgiReferenceRequest& reference);
    void LogFrameTimingsNow() const;

    void CreateDeviceResources();
    void DestroyDeviceResources();
    EnvironmentDescriptorBindings BuildEnvironmentBindings() const;
    VkSampler EquirectangularSampler() const;
    EnvironmentMode EffectiveEnvironmentMode(const SceneEnvironment& environment) const;
    // Starts, finishes or skips the background decode of the scene's HDRI; installs it when ready.
    void UpdateEnvironmentMap(const SceneEnvironment& environment);
    // Loads the scene's minimap picture when its path changes; the editor UI draws it as
    // kMinimapTextureId.
    void UpdateMinimapTexture(const std::string& path);
    void ReleaseMinimapTexture();
    void CreateSwapchainResources();
    void CreateScenePasses();
    IScenePass* FindScenePass(ScenePassId id) const;
    void DestroySwapchainResources();
    void CreateDescriptorResources();
    void DestroyDescriptorResources();
    void RecreateSwapchain();
    // The extent a swapchain made now would have: 0 x 0 while the window is minimized.
    VkExtent2D WantedSwapchainExtent() const;
    // The render and output sizes for a viewport of this size, and the DLSS mode that upscales
    // between them (Off: the same size, the engine's TAA). Makes or drops the DLSS feature to match.
    struct SceneExtents
    {
        VkExtent2D render{};
        VkExtent2D output{};
        DlssMode dlss = DlssMode::Off;
        DlssPreset dlssPreset = DlssPreset::Default;
    };
    SceneExtents ResolveSceneExtents(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug);
    void SyncSceneTargets(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug);
    // Resizes the scene targets (and DLSS) for the viewport extent. Throws VulkanError when out of
    // device memory, leaving the targets to be rebuilt before the next draw.
    void ApplySceneExtent(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug);
    // Builds the GPU content for the frame's submeshes and swaps it in. Transactional: when it
    // throws, the previous content, textures and descriptor sets are untouched and still drawable.
    void UploadSceneResources(const RenderFramePacket& frame);
    // UploadSceneResources for a change made while the editor runs. Running out of GPU memory is
    // reported in the editor and leaves the previous content on screen instead of ending the
    // program; any other failure still propagates.
    void UploadSceneResourcesOrKeepPrevious(const RenderFramePacket& frame);
    // A renderables change: queues the texture files it needs that are not already on the GPU, and
    // marks an upload pending. The upload itself happens in PumpSceneUpload.
    void RequestSceneUpload(const RenderFramePacket& frame);
    // Called every frame: uploads a few textures the workers finished into the staged set and, once
    // a pending change has every texture it needs, commits it with UploadSceneResourcesOrKeepPrevious.
    // Also keeps m_sceneUploadStatus current.
    void PumpSceneUpload(const RenderFramePacket& frame);
    // Forgets a pending change's staged textures and failures, after it ran out of memory.
    void AbandonPendingTextures();
    std::unique_ptr<VulkanTexture> UploadPreparedTexture(
        const PreparedTexture& prepared,
        TextureUsage usage,
        VulkanUploadBatch& uploadBatch);
    // The previous content may still name entities the frame no longer has (deleted, or their model
    // removed). They are not drawn any more, so their submeshes are dropped.
    void DropSubmeshesOfRemovedEntities(const RenderFramePacket& frame);
    void ApplyRenderContent(
        std::vector<std::shared_ptr<const RenderSubmesh>> newRenderSubmeshes,
        size_t keptSubmeshCount,
        const RenderFramePacket& frame);
    // Destroys the stored textures no live submesh names (after the material sets that named them).
    void DropUnreferencedTextures();
    // models is parallel to m_renderSubmeshes: this frame's model matrix of each submesh. Submeshes
    // whose bounding sphere is outside the frustum of viewProjection get no draw item.
    std::vector<VulkanDrawItem> BuildDrawItems(
        uint32_t imageIndex,
        std::span<const glm::mat4> models,
        const glm::mat4& viewProjection,
        const glm::mat4& view) const;
    std::vector<ShadowDrawItem> BuildShadowDrawItems(uint32_t imageIndex, std::span<const glm::mat4> models) const;
    // One submesh's draw item and sort key, unless the frustum culls it; BuildDrawItems' loop body.
    static void AppendDrawItem(
        const RenderSubmesh& renderSubmesh,
        const glm::mat4& model,
        const ViewFrustum& frustum,
        const glm::mat4& view,
        std::vector<VulkanDrawItem>& items,
        std::vector<MaterialDrawSortKey>& sortKeys);
    // How a submesh casts shadows, and its caster: BuildShadowDrawItems' two passes.
    enum class ShadowCaster
    {
        None,
        Opaque,
        Masked,
    };
    static ShadowCaster ClassifyShadowCaster(const RenderSubmesh& renderSubmesh);
    static void FillShadowDrawItem(const RenderSubmesh& renderSubmesh, const glm::mat4& model, ShadowDrawItem& item);
    // The selected entity's submeshes the selection outline draws (ScenePassFrameContext::
    // selectionDrawItems): every one but its decals whose bounds reach the frustum of viewProjection.
    // Empty without a selection (entt::null).
    std::vector<ShadowDrawItem> BuildSelectionDrawItems(
        entt::entity selected,
        std::span<const glm::mat4> models,
        const glm::mat4& viewProjection) const;
    void RecordTransitions(
        VkCommandBuffer commandBuffer,
        const RenderPassIo& io,
        const ScenePassFrameContext& frame);
    void RecordScenePasses(
        VkCommandBuffer commandBuffer,
        const ScenePassFrameContext& frame,
        std::span<const ScenePassId> passOrder);
    void RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex, ImDrawData* drawData) const;
    // Meters the histogram the given frame slot last wrote and moves the frame camera's EV100
    // toward it. Must run after AcquireNextImage has waited on that slot's fence.
    void UpdateAutoExposure(RenderFramePacket& frame, uint32_t frameSlot);
    // Adapts the white point toward this frame's illuminant estimate and returns the balance the
    // tone mapping pass applies (identity when auto white balance is off).
    glm::mat3 UpdateWhiteBalance(RenderFramePacket& frame);
    // Logs when the number of lights left out by the light limit changes.
    void ReportDroppedLights(uint32_t droppedCount);
    void ReportDroppedClusterLights(uint32_t droppedCount);
    void ReportDroppedLocalShadows(uint32_t droppedCount);

    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    // NVIDIA DLSS: always made, available only with the SDK on a device and driver that run it.
    std::unique_ptr<VulkanDlss> m_dlss;
    // The DLSS mode the scene targets were last sized for (Off while the engine's TAA resolves), and
    // whether DLSS's next evaluation throws its history away.
    DlssMode m_activeDlssMode = DlssMode::Off;
    DlssPreset m_activeDlssPreset = DlssPreset::Default;
    bool m_dlssResetPending = true;
    // Set while the viewport's own size ran out of device memory: the smaller size rendered instead.
    struct SceneTargetFallback
    {
        RenderExtent requested;
        RenderExtent used;
    };
    std::optional<SceneTargetFallback> m_sceneTargetFallback;
    std::vector<std::shared_ptr<const RenderSubmesh>> m_renderSubmeshes;
    // m_renderSubmeshes by revision, for the next upload to keep.
    std::unordered_map<uint64_t, std::shared_ptr<const RenderSubmesh>> m_liveSubmeshes;
    std::unique_ptr<VulkanMaterialSetCache> m_materialSets;
    // Draw slots: freed ones are handed out again first, and the watermark is how many the per-draw
    // buffers must hold.
    uint32_t AcquireDrawSlot();
    void ReleaseDrawSlot(uint32_t slot);
    std::vector<uint32_t> m_freeDrawSlots;
    uint32_t m_drawSlotWatermark = 0;
    // Every texture the content draws with, by cache key ("path|color", "__id__|linear"), counted by
    // the submeshes that name it, so a change of content touches only the textures it adds or drops.
    std::unordered_map<std::string, StoredTexture> m_textureStore;
    // Prepares texture files on worker threads; see RequestSceneUpload and PumpSceneUpload.
    std::unique_ptr<TexturePreparationQueue> m_texturePreparation;
    // Textures prepared and uploaded for a change that has not committed yet, by cache key. The
    // upload moves the ones it uses into m_textureStore.
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
    // The scene as compute shaders trace it (DDGI, and with hardware ray tracing the ray traced
    // effects), and the white texture its texture table names where no material's is.
    std::unique_ptr<VulkanRayScene> m_rayScene;
    std::unique_ptr<VulkanTexture> m_rayDefaultTexture;
    // The DDGI probes, the CPU's schedule of their updates, the level layout their data belongs to
    // (count and base spacing: another one invalidates every probe) and the frame index that seeds
    // their ray rotations.
    std::unique_ptr<VulkanDdgi> m_ddgi;
    DdgiProbeScheduler m_ddgiScheduler;
    // The ray scene instances the probe rays skip while they move.
    DdgiMovingInstances m_ddgiMovingInstances;
    // Faster blending after the lighting changes, timed from the previous frame.
    DdgiLightingWatch m_ddgiLighting;
    // Counts the ray scene's installs: probes that recorded another judge their surroundings afresh.
    uint32_t m_ddgiGeometryEpoch = 0;
    // VulkanDdgi::TakeFeedback's output, kept to reuse the allocations.
    std::vector<uint32_t> m_ddgiFeedbackSchedule;
    std::vector<uint32_t> m_ddgiFeedback;
    glm::vec2 m_ddgiLayout{0.0f};
    uint32_t m_ddgiFrameIndex = 0;
    std::unique_ptr<VulkanTexture> m_environmentBrdfLut;
    // The area lights' LTC tables (ltc_table.h), one mip each, RGBA32F.
    std::unique_ptr<VulkanTexture> m_ltcInverseMatrices;
    std::unique_ptr<VulkanTexture> m_ltcAmplitudes;
    // Set 0 binding 6 when no HDRI is loaded.
    std::unique_ptr<VulkanTexture> m_defaultEnvironmentMap;
    // The loaded HDRI and the scene path it came from; empty until one loads.
    std::unique_ptr<VulkanTexture> m_environmentMap;
    std::string m_environmentMapPath;
    // The scene's minimap picture (SceneMinimap) as ImGui samples it, and the path it was loaded for;
    // the texture is null when the scene has none or its file would not load.
    std::unique_ptr<VulkanTexture> m_minimapTexture;
    VkDescriptorSet m_minimapBinding = VK_NULL_HANDLE;
    std::string m_minimapPath;
    // A decode running on a worker thread, and the path it decodes.
    // A decoded HDRI and its SH, prepared together on the worker thread.
    struct PreparedEnvironmentMap
    {
        FloatTextureData image;
        ShCoefficients sh{};
    };
    TaskFuture<PreparedEnvironmentMap> m_pendingEnvironmentMap;
    // The loaded HDRI's unrotated radiance SH, at intensity 1.
    ShCoefficients m_environmentMapSh{};
    std::string m_pendingEnvironmentMapPath;
    // The last path that failed, so a bad file is reported once rather than every frame.
    std::string m_failedEnvironmentMapPath;
    // The swapchain image the last submitted frame drew into, for CaptureViewport.
    std::optional<uint32_t> m_lastRecordedImageIndex;
    // A video recording's frames on their way back from the GPU; made when the first is wanted.
    std::unique_ptr<VulkanVideoReadback> m_videoReadback;
    // Hands the recording the slot's frame once its fence has been waited on, if it holds one.
    void SubmitVideoFrame(uint32_t frameSlot);
    // What the last drawn frame showed, for CaptureDdgiReference: its unjittered view-projection and
    // camera, pre-exposure, frame slot and debug view, the directional lights as they reached the
    // scene, and its sky, which the reference supports only when uniform.
    struct ReferenceFrame
    {
        glm::mat4 viewProjection{1.0f};
        glm::vec3 cameraPosition{0.0f};
        float preExposure = 1.0f;
        uint32_t frameSlot = 0;
        GBufferDebugView view = GBufferDebugView::Off;
        std::vector<ReferenceLight> lights;
        glm::vec3 skyRadiance{0.0f};
        bool uniformSky = false;
        bool ddgiEnabled = false;
        // The DDGI levels' grids and the lookup's normal and view bias, in spacings.
        uint32_t ddgiLevels = 0;
        std::array<glm::ivec3, kDdgiMaxLevels> ddgiOrigins{};
        std::array<float, kDdgiMaxLevels> ddgiSpacings{};
        glm::vec2 ddgiBias{0.0f};
    };
    ReferenceFrame m_referenceFrame;
    // The finest level's nearest probes against the reference along six axes, in
    // ddgi_reference_capture.cpp. device names the device and queue to read back with.
    void CompareDdgiProbes(const std::filesystem::path& prefix, const ImageCaptureRequest& device, uint32_t samples);
    void ExplainDdgiLookup(const ImageCaptureRequest& device, glm::vec3 P, glm::vec3 N, glm::vec3 V);
    // Material textures' samplers, shared by every descriptor that asks for the same settings.
    std::unique_ptr<VulkanSamplerCache> m_samplerCache;
    // The scene behind transmissive surfaces, bound in set 0 (VulkanTransmissionCopyPass fills it).
    std::unique_ptr<VulkanTransmissionImage> m_transmissionImage;
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
    TemporalHistory m_rtShadowHistory;
    TemporalHistory m_giHistory;
    TemporalHistory m_ssrHistory;
    TemporalHistory m_taaHistory;
    // The pre-exposure the TAA history was written with; 0 before any frame wrote it.
    float m_taaHistoryPreExposure = 0.0f;
    // Advances once per frame that jitters; picks the frame's offset in the TAA jitter sequence.
    uint32_t m_taaFrameIndex = 0;
    // How far the clouds have moved, run on by every frame's time (engine/renderer/volumetric_clouds.h).
    CloudMotion m_cloudMotion;
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
    // Owned by m_scenePasses like the exposure pass; its images back set 0 bindings 19 and 20.
    VulkanScatterPass* m_scatterPass = nullptr;
    std::unique_ptr<VulkanPipelineSet> m_forwardPipelines;
    // triangle.frag under kScatterPrepass, against the scatter pass's render pass.
    std::unique_ptr<VulkanPipelineSet> m_scatterPipelines;
    std::unique_ptr<VulkanPipelineSet> m_geometryPipelines;
    // gbuffer.frag as a deferred decal, against the geometry pass.
    std::unique_ptr<VulkanPipelineSet> m_decalPipelines;
    std::unique_ptr<VulkanCommandContext> m_commandContext;
    std::unique_ptr<VulkanImGuiLayer> m_imguiLayer;
    // GPU time per pass, and the CPU's time per frame outside the waits, for LogFrameTimings.
    std::unique_ptr<VulkanGpuTimer> m_gpuTimer;
    // Records the shadow layers' and the material passes' large draw lists on the task system;
    // null with --no-parallel-recording.
    std::unique_ptr<VulkanParallelRecorder> m_parallelRecorder;
    std::vector<double> m_cpuFrameMs;
    std::vector<double> m_cpuWaitMs;
    uint32_t m_cpuFrameCursor = 0;
    // The frame's CPU time by stage, logged with the frame timings: the render thread's, and the
    // main thread's.
    CpuStageTimer m_cpuStages;
    CpuStageTimer m_mainStages;
    std::vector<double> m_mainFrameMs;
    uint32_t m_mainFrameCursor = 0;

    // The render thread, and the two frames it alternates between: the main thread builds one while
    // the render thread draws the other.
    std::unique_ptr<RenderThread> m_renderThread;
    std::array<RenderFramePacket, 2> m_framePackets;
    uint64_t m_frameSerial = 0;
    // The last frame whose renderables changed: until the render thread has drawn it, the scene
    // counts as loading (RendererSharedState::rayScenePending).
    uint64_t m_lastContentSerial = 0;
    // Set by the render thread when acquire or present found the swapchain out of date or
    // suboptimal. It then skips frames until the main thread rebuilds the swapchain.
    std::atomic<bool> m_swapchainOutOfDate{false};
    std::mutex m_feedbackMutex;
    RenderFeedback m_feedback;
    // The render thread's own state behind the feedback.
    std::string m_sceneUploadStatus;
    std::optional<bool> m_outOfMemoryChange;
    // The EV100 auto exposure reached; the main thread's camera trails it by a frame.
    std::optional<float> m_renderExposureEv100;
    // The main thread's copy of RenderFeedback::minimapLoaded.
    bool m_minimapAvailable = false;
};
}
