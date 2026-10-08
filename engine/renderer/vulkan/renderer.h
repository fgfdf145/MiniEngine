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
#include "path_trace_pass.h"
#include "ray_scene.h"
#include "retire_queue.h"
#include "rt_shadow_pass.h"
#include "restir_pt_pass.h"
#include "ddgi.h"
#include "scene_render_targets.h"
#include "scene_view.h"
#include "selection_outline_pass.h"
#include "skinning_pass.h"
#include "toon_pass.h"
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
#include <fstream>
#include <engine/renderer/volumetric_clouds.h>
#include <engine/asset/texture_preparation.h>
#include <engine/core/threading/render_thread.h>
#include <engine/renderer/cpu_stage_timer.h>
#include <engine/renderer/taa_jitter.h>
#include <engine/renderer/view_frustum.h>
#include <engine/renderer/temporal_history.h>
#include <engine/renderer/motion_history.h>
#include <engine/renderer/path_tracing.h>
#include <engine/platform/display/display_hdr.h>
#include <engine/renderer/local_shadows.h>
#include <engine/renderer/render_features.h>

#include <array>
#include <deque>
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
    TextureDescriptorBinding rayNormal;
    // The draw slot (firstInstance) holding this submesh's material, texture transforms, previous model
    // matrix and ray material, from the commit that first draws it until the one that drops it. Set at
    // commit, hence mutable: everything else is fixed once made.
    static constexpr uint32_t kNoDrawSlot = UINT32_MAX;
    mutable uint32_t drawSlot = kNoDrawSlot;
    // The last commit that drew it (VulkanRenderer::m_commitSerial): a commit tells the draws it keeps
    // from the ones it drops by this, without a set of every draw.
    mutable uint64_t commitSerial = 0;
    GpuMaterialData material;
    GpuTextureTransforms textureTransforms;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool decal = false;
    // An anime character material (CpuRenderSubmesh::toon), which the toon passes shade.
    std::shared_ptr<const ToonMaterialData> toon;
    // Skinned: the skinning pass deforms its buffers by the entity's joint palette, from
    // paletteOffset, every frame (CpuRenderSubmesh::skinned and the rest).
    bool skinned = false;
    uint32_t paletteOffset = 0;
    uint32_t jointCount = 0;
    int32_t toonHeadJoint = -1;
    // A tyre (MeshData::deformable): the skinning pass deforms its buffers by its TyreDeformation from
    // the entity's (RenderTransformSnapshot::GetTyreDeformations), every frame.
    bool tyre = false;
    // The skinning pass's descriptor set for its buffers, made with the submesh.
    mutable VkDescriptorSet skinningSet = VK_NULL_HANDLE;
    glm::vec3 localBoundsCenter{0.0f};
    float localBoundsRadius = 0.0f;
    // CpuRenderSubmesh::castShadows and drawDistance.
    bool castShadows = true;
    DrawDistanceRange drawDistance;
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
    void FlushQuadVideoFrames() override;
    void RunWithRenderIdle(const std::function<void()>& work) override;

  private:
    // Main thread: takes what the render thread reported of its last frame (exposure, upload status).
    void ApplyRenderFeedback();
    // Main thread, after ImGui::Render: makes, updates or destroys the textures ImGui asked for.
    void ApplyImGuiTextureRequests(const ImDrawData& drawData);
    // Main thread: everything the render thread will read of this frame.
    void BuildFramePacket(RenderFramePacket& packet, bool contentChanged, RenderExtent viewportExtent);
    // A frame's work that every view shares, done once before the views (RenderFrame): the lights,
    // every draw's model matrix and the shadow casters, the sky, the probes, the local shadow atlas,
    // the skinning and the white balance.
    struct SharedFrameState
    {
        uint32_t imageIndex = 0;
        uint32_t frameSlot = 0;
        SceneLightSelection lightSelection;
        std::vector<GpuLightData> selectedLights;
        uint32_t directionalLightCount = 0;
        int32_t shadowLightIndex = -1;
        std::vector<glm::mat4> models;
        std::vector<MotionKey> motionKeys;
        std::vector<uint32_t> drawSlots;
        std::vector<ShadowDrawItem> shadowDrawItems;
        uint64_t shadowCasterKey = 0;
        EnvironmentMode environmentMode = EnvironmentMode::None;
        AtmosphereParameters atmosphereParameters{};
        std::optional<AtmosphereSun> sun;
        DdgiUniformData ddgiData{};
        std::vector<LocalShadowTile> localShadowTiles;
        std::vector<GpuLocalShadowTile> gpuShadowTiles;
        std::vector<VulkanSkinningPass::Dispatch> skinningDispatches;
        glm::mat3 whiteBalance{1.0f};
        // The viewport's: a capture view takes away DLSS (PrepareView).
        RenderCapabilities capabilities;
    };
    // One view's frame, prepared before recording: its uniforms written, its draws, its cascades,
    // and the frame context its passes record with (which points into the draws, so this stays put).
    struct PreparedView
    {
        VulkanSceneView* view = nullptr;
        ScenePassFrameContext frame{};
        std::span<const ScenePassId> passOrder;
        std::optional<ShadowCascadePlan> shadowPlan;
        EnvironmentUniformData environment{};
        std::vector<VulkanDrawItem> drawItems;
        std::vector<VulkanDrawItem> decalDrawItems;
        std::vector<VulkanDrawItem> toonDrawItems;
        std::vector<VulkanDrawItem> scatterDrawItems;
        std::vector<ShadowDrawItem> selectionDrawItems;
    };
    // Render thread: draws one frame from its packet, every capture view's then the viewport's.
    void RenderFrame(RenderFramePacket& frame);
    // A view's frame from camera, whose EV UpdateAutoExposure has adapted. The viewport's alone runs
    // DLSS, path tracing, the selection outline and the G-buffer debug views.
    std::unique_ptr<PreparedView> PrepareView(
        VulkanSceneView& view,
        const Camera& camera,
        const ViewportMatrices& viewportMatrices,
        bool viewport,
        const SharedFrameState& shared,
        RenderFramePacket& packet);
    // The camera block's environment for a camera at cameraPosition, its clouds' march jitter at
    // taaFrameIndex.
    EnvironmentUniformData BuildViewEnvironment(
        const SharedFrameState& shared,
        const RenderFramePacket& packet,
        const glm::vec3& cameraPosition,
        uint32_t taaFrameIndex) const;
    void PublishFeedback(const RenderFramePacket& frame);
    // The device-local memory now (docs/design/2026-10-07-vram-budget-design.md). Render thread.
    GpuMemoryReport MeasureGpuMemory(const RenderFramePacket& frame) const;
    void CaptureViewportNow(const std::filesystem::path& path);
    // In ddgi_reference_capture.cpp.
    void CaptureDdgiReferenceNow(const DdgiReferenceRequest& reference);
    void LogFrameTimingsNow() const;

    void CreateDeviceResources();
    void DestroyDeviceResources();
    // Set 0's environment bindings for a view: the shared images, and the view's own scatter images,
    // aerial perspective and clouds.
    EnvironmentDescriptorBindings BuildEnvironmentBindings(const VulkanSceneView& view) const;
    // Set 0 bindings 29 to 31 for a view: its path traced layer's images once they exist, the DFG
    // table in their place before.
    void PathTraceLayerBindings(
        const VulkanSceneView& view, TextureDescriptorBinding& depth, TextureDescriptorBinding& diffuse, TextureDescriptorBinding& specular) const;
    // A path traced frame's forward-shaded surfaces' layer (path_trace_layer_pass.h): its images,
    // made, moved to their resting layout and named in set 0 the first time; true when the frame
    // can trace it.
    bool PreparePathTraceLayer(VulkanSceneView& view);
    // A view's set 0 for drawCapacity draws, with every live draw's material written in.
    std::unique_ptr<VulkanUniformBuffer> CreateViewUniformBuffer(const VulkanSceneView& view, uint32_t drawCapacity) const;
    VkSampler EquirectangularSampler() const;
    EnvironmentMode EffectiveEnvironmentMode(const SceneEnvironment& environment) const;
    // Starts, finishes or skips the background decode of the scene's HDRI; installs it when ready.
    void UpdateEnvironmentMap(const SceneEnvironment& environment);
    // Loads the scene's minimap picture when its path changes; the editor UI draws it as
    // kMinimapTextureId.
    void UpdateMinimapTexture(const std::string& path);
    void ReleaseMinimapTexture();
    void CreateSwapchainResources();
    // Copies the display monitor's latest answer (main thread).
    void UpdateDisplayReport();
    // Whether the output settings ask for HDR10 on this display, and the UI white it would use.
    bool WantsHdrSwapchain() const;
    float WantedUiWhiteNits() const;
    // Gives the HDR10 swapchain the content's luminance range when it changed (render thread).
    void ApplyHdrMetadata(const DisplayOutput& display);
    // A view's passes on its targets; the viewport's also build the material pipelines every view
    // draws with (the views' render passes are alike, so compatible).
    void CreateScenePasses(VulkanSceneView& view);
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
        // The DLSS feature is ray reconstruction.
        bool rayReconstruction = false;
    };
    SceneExtents ResolveSceneExtents(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug);
    void SyncSceneTargets(RenderExtent viewportExtent, const RenderDebugSettings& renderDebug);
    // Rebuilds a view's targets at another size, and what was made from them; its histories start
    // over. Waits for the device.
    void RebuildViewTargets(VulkanSceneView& view, VkExtent2D render, VkExtent2D output);
    // Keeps one capture view per camera the frame names, each at its camera's size: made, resized
    // or dropped (waiting for the device) as the cameras come and go.
    void SyncCaptureViews(std::span<const SceneCaptureView> cameras);
    std::unique_ptr<VulkanSceneView> CreateCaptureView(VkExtent2D extent);
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
    // Drops what a pending change prepared for its new meshes (retired: uploads may be running).
    void ReleasePreparedBuffers();
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
    // whose bounding sphere is outside the frustum of viewProjection, or whose centre is outside their
    // draw distance from the camera of view, get no draw item.
    std::vector<VulkanDrawItem> BuildDrawItems(
        uint32_t imageIndex,
        std::span<const glm::mat4> models,
        const glm::mat4& viewProjection,
        const glm::mat4& view) const;
    // The shadow casters: every submesh that casts shadows and that the main camera, at cameraPosition,
    // is within the draw distance of, so a level of detail casts only where it is drawn.
    std::vector<ShadowDrawItem> BuildShadowDrawItems(
        uint32_t imageIndex,
        std::span<const glm::mat4> models,
        const glm::vec3& cameraPosition) const;
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
    static ShadowCaster ClassifyShadowCaster(const RenderSubmesh& renderSubmesh, const glm::mat4& model, const glm::vec3& cameraPosition);
    // Whether a camera at cameraPosition is within the submesh's draw distance of its bounds' centre.
    static bool WithinDrawDistance(const RenderSubmesh& renderSubmesh, const glm::mat4& model, const glm::vec3& cameraPosition);
    static void FillShadowDrawItem(const RenderSubmesh& renderSubmesh, const glm::mat4& model, ShadowDrawItem& item);
    // The selected entity's submeshes the selection outline draws (ScenePassFrameContext::
    // selectionDrawItems): every one but its decals whose bounds reach the frustum of viewProjection
    // and that a camera at cameraPosition draws. Empty without a selection (entt::null).
    std::vector<ShadowDrawItem> BuildSelectionDrawItems(
        entt::entity selected,
        std::span<const glm::mat4> models,
        const glm::mat4& viewProjection,
        const glm::vec3& cameraPosition) const;
    void RecordTransitions(
        VkCommandBuffer commandBuffer,
        VulkanSceneView& view,
        const RenderPassIo& io,
        const ScenePassFrameContext& frame);
    // timer, when given, marks each pass.
    void RecordScenePasses(
        VkCommandBuffer commandBuffer,
        VulkanSceneView& view,
        const ScenePassFrameContext& frame,
        std::span<const ScenePassId> passOrder,
        VulkanGpuTimer* timer);
    // The path tracer's per-frame state, once frame.pathTracing says whether it runs: its images the
    // first time, whether its image stands still and so how long a history a pixel averages, the
    // history's ping-pong and pre-exposure scale, and its status line.
    void UpdatePathTracing(
        ScenePassFrameContext& frame,
        const RenderFramePacket& packet,
        std::span<const GpuLightData> lights,
        const glm::vec3& ambientLuminance,
        const EnvironmentUniformData& environment,
        float preExposure);
    void RecordEditorLayer(VkCommandBuffer commandBuffer, uint32_t imageIndex, ImDrawData* drawData) const;
    // Meters the histogram the given frame slot last wrote and moves the frame camera's EV100
    // toward it. Must run after AcquireNextImage has waited on that slot's fence.
    void UpdateAutoExposure(VulkanSceneView& view, Camera& camera, const RenderFramePacket& frame, uint32_t frameSlot);
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
    bool m_activeDlssRayReconstruction = false;
    bool m_dlssResetPending = true;
    std::vector<std::shared_ptr<const RenderSubmesh>> m_renderSubmeshes;
    // m_renderSubmeshes by revision, for the next upload to keep.
    std::unordered_map<uint64_t, std::shared_ptr<const RenderSubmesh>> m_liveSubmeshes;
    std::unique_ptr<VulkanMaterialSetCache> m_materialSets;
    // Draw slots: freed ones are handed out again first (once the frames that drew from them have
    // finished), and the watermark is how many the per-draw buffers must hold.
    uint32_t AcquireDrawSlot();
    void ReleaseDrawSlot(uint32_t slot);
    // What the frames in flight may still use, freed once they have finished (VulkanRetireQueue):
    // textures, submeshes' buffers, descriptor sets and draw slots a change of content drops.
    void Retire(std::function<void()> release);
    VulkanRetireQueue m_retireQueue;
    std::vector<uint32_t> m_freeDrawSlots;
    uint32_t m_drawSlotWatermark = 0;
    uint64_t m_commitSerial = 0;
    // The GPU buffers made for each CPU mesh, for a new submesh of a mesh already uploaded: kept
    // between commits (rebuilding it from every live submesh cost 2.5 ms a change on a map). The
    // mesh is held weakly too, as an address alone could be a new mesh made where a freed one was.
    struct MeshBuffers
    {
        std::weak_ptr<const MeshData> mesh;
        std::weak_ptr<VulkanBuffer> buffer;
    };
    std::unordered_map<const MeshData*, MeshBuffers> m_meshBuffers;
    // A pending change's new meshes, whose GPU buffers PumpSceneUpload makes a few at a time before the
    // commit (a cell of hundreds made in the commit took 9 ms of one frame), and the buffers made so
    // far, held until the commit uses them.
    std::deque<std::shared_ptr<const MeshData>> m_meshesToUpload;
    // The mesh is held so that its address stays its own until the commit.
    struct PreparedBuffers
    {
        std::shared_ptr<const MeshData> mesh;
        std::shared_ptr<VulkanBuffer> buffer;
    };
    std::unordered_map<const MeshData*, PreparedBuffers> m_preparedBuffers;
    // Textures whose last reference went (or that a commit stored), the only ones DropUnreferencedTextures
    // looks at.
    std::vector<std::string> m_unreferencedTextureKeys;
    // Every texture the content draws with, by cache key ("path|color", "__id__|linear"), counted by
    // the submeshes that name it, so a change of content touches only the textures it adds or drops.
    std::unordered_map<std::string, StoredTexture> m_textureStore;
    // Prepares texture files on worker threads; see RequestSceneUpload and PumpSceneUpload.
    std::unique_ptr<TexturePreparationQueue> m_texturePreparation;
    // Textures prepared and uploaded for a change that has not committed yet, by cache key. The
    // upload moves the ones it uses into m_textureStore.
    std::unordered_map<std::string, std::unique_ptr<VulkanTexture>> m_stagedTextures;
    // The batches that staged them and that uploaded a commit's buffers, submitted without a wait and
    // dropped once the GPU has run them. Clearing the list waits for the rest: before anything they
    // upload into is destroyed unused.
    std::vector<std::unique_ptr<VulkanUploadBatch>> m_uploadBatches;
    // Their staging memory, kept between batches; made with the device, gone before it.
    std::unique_ptr<VulkanStagingChunkPool> m_stagingChunkPool;
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
    // Device lifetime too: its image has a fixed size and is shared by every frame in flight and
    // every view, and every VulkanUniformBuffer binds it into set 0.
    std::unique_ptr<VulkanLocalShadowPass> m_localShadowPass;
    // Device lifetime as well, for the same reasons: fixed-size images shared by every frame in
    // flight and bound into set 0 by every VulkanUniformBuffer.
    std::unique_ptr<VulkanAtmosphere> m_atmosphere;
    // The sky prefiltered for the specular lobe, and the DFG table it is weighted by.
    std::unique_ptr<VulkanEnvironmentProbe> m_environmentProbe;
    // The scene as compute shaders trace it (DDGI, and with hardware ray tracing the ray traced
    // effects), and the white texture its texture table names where no material's is.
    std::unique_ptr<VulkanRayScene> m_rayScene;
    // Poses the skinned submeshes at the start of every frame (skinning_pass.h).
    std::unique_ptr<VulkanSkinningPass> m_skinningPass;
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
    // How much light the probes' lighting epoch started with (DdgiLightLevel): a new epoch that differs
    // from it by more than kDdgiLightingJump either way clears the probes.
    float m_ddgiLightLevel = 0.0f;
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
    // The quad recording's canvases on their way back, and the same for them; their cameras' names
    // are written on before they go.
    std::unique_ptr<VulkanVideoReadback> m_quadReadback;
    void SubmitQuadVideoFrame(uint32_t frameSlot);
    void SubmitQuadCanvas(VulkanVideoReadback::Frame& canvas);
    // Where each capture view's picture goes on the quad recording's canvas this frame; empty
    // without a recording, or while a view is not at its tile's size.
    std::vector<VulkanVideoReadback::MosaicTile> BuildQuadMosaicTiles(uint32_t imageIndex) const;
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
        // The frame was path traced: SceneGi holds the traced diffuse light, which the reference
        // comparison then checks instead of the probes.
        bool pathTraced = false;
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
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanRenderPass> m_renderPass;
    // The viewport's camera: its targets, passes, frame sets and histories (scene_view.h). Its
    // shadow pass is made with the device; the rest with the swapchain.
    VulkanSceneView m_view;
    // The quad recording's cameras, in the frame's order (RenderFramePacket::captureViews).
    std::vector<std::unique_ptr<VulkanSceneView>> m_captureViews;
    // The sun and sky references auto exposure meters against, gathered while recording the
    // previous frame; shared by every view.
    ExposureReferences m_exposureReferences;
    // Auto white balance (see white_balance.h): the references gathered with the exposure ones, and
    // the adapted white point; empty until the first balanced frame.
    WhiteBalanceReferences m_whiteBalanceReferences;
    std::optional<glm::vec2> m_adaptedWhiteXy;
    // What the OS says about the window's display, polled on its own thread; the main thread copies
    // the latest answer at the start of each frame.
    std::unique_ptr<platform::display::DisplayHdrMonitor> m_displayMonitor;
    // MINIENGINE_FRAME_TIMES=<file>: each present's time in microseconds, one per line (render thread),
    // to measure frame pacing.
    std::ofstream m_frameTimesFile;
    platform::display::DisplayHdrInfo m_displayInfo;
    DisplayReport m_displayReport;
    // The HDR output the current swapchain was created for, and the UI white ImGui's HDR shader was
    // built with; a different wish recreates them.
    bool m_swapchainHdrRequested = false;
    float m_swapchainUiWhiteNits = kDefaultUiWhiteNits;
    // The HDR metadata last given to the swapchain (render thread); reset with the swapchain.
    std::optional<DisplayOutput> m_appliedHdrMetadata;
    uint32_t m_droppedLightCount = 0;
    uint32_t m_droppedClusterLightCount = 0;
    uint32_t m_droppedLocalShadowCount = 0;
    // What ReSTIR PT's accumulate mode compares to tell a still camera (the viewport's only).
    glm::mat4 m_restirPtAccumulationView{0.0f};
    uint32_t m_restirPtAccumulatedFrames = 0;
    uint64_t m_restirPtAccumulationEpochs = 0;
    // Whether the viewport's path traced image is standing still; reset where its histories are.
    PathTraceAccumulation m_pathTraceAccumulation;
    // The ray scene's install count the accumulation last saw: a new one is a scene change.
    uint32_t m_pathTraceGeometryEpoch = 0;
    // What the Graphics Debug window says of path tracing: the render thread's line, and the main
    // thread's copy from the feedback.
    std::string m_pathTracingStatus;
    std::string m_pathTracingStatusShown;
    // How far the clouds have moved, run on by every frame's time (engine/renderer/volumetric_clouds.h).
    CloudMotion m_cloudMotion;
    std::unique_ptr<VulkanPipelineSet> m_forwardPipelines;
    // triangle.frag under kScatterPrepass, against the scatter pass's render pass.
    std::unique_ptr<VulkanPipelineSet> m_scatterPipelines;
    std::unique_ptr<VulkanPipelineSet> m_geometryPipelines;
    // gbuffer.frag as a deferred decal, against the geometry pass.
    std::unique_ptr<VulkanPipelineSet> m_decalPipelines;
    // gbuffer.frag's two passes of the path traced layer (kLayerPass 1 and 2), against
    // VulkanPathTraceLayerPass's render passes; null where the device cannot blend R32F.
    std::unique_ptr<VulkanPipelineSet> m_pathTraceLayerDepthPipelines;
    std::unique_ptr<VulkanPipelineSet> m_pathTraceLayerSurfacePipelines;
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
    GpuMemoryReport m_gpuMemory;
    // The main thread's copy of RenderFeedback::minimapLoaded.
    bool m_minimapAvailable = false;
};
}
