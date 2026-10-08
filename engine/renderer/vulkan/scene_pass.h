#pragma once

#include "command.h"
#include "common.h"
#include "pipeline_set.h"
#include "render_target_layout.h"
#include "scene_pass_order.h"
#include "scene_render_targets.h"
#include "shadow_pass.h"

#include <engine/renderer/temporal_history.h>
#include <engine/renderer/exposure.h>
#include <engine/renderer/glare.h>
#include <engine/renderer/render_types.h>

#include <span>

namespace me
{

class VulkanParallelRecorder;
class VulkanDlss;
class VulkanGpuTimer;

// Which draw items the forward pass records, and whether it owns the frame. The deferred order
// gives it only Blend items to composite over the lighting result; the forward-only comparison
// order gives it everything. It lives in the frame context because the switch flips between
// frames while the pass object stays the same.
enum class ForwardDrawFilter
{
    BlendOnly,
    All
};

// Everything a pass may need about the frame being recorded. Passes hold no per-frame state of
// their own, so a pass object is reusable across frames and owns only its render pass and
// framebuffers.
struct ScenePassFrameContext
{
    uint32_t imageIndex = 0;
    uint32_t frameSlot = 0;
    // The render size, which the scene is drawn and lit at.
    VkExtent2D extent{};
    // The output size (SceneRenderTargets::GetOutputExtent): the temporal resolve's result and every
    // pass after it. Larger than extent while DLSS upscales, the same otherwise.
    VkExtent2D outputExtent{};
    // Every draw item for the frame: Opaque and Mask first in pipeline variant order, then Blend
    // back to front, the partition BuildMaterialDrawOrder produces. blendDrawItemBegin is the
    // index of the first Blend item, so the two halves are views of one vector, never copies.
    std::span<const VulkanDrawItem> drawItems;
    size_t blendDrawItemBegin = 0;
    // The first Opaque or Mask item the forward pass shades; they run up to
    // transmissiveDrawItemBegin.
    size_t forwardShadedDrawItemBegin = 0;
    // The first transmissive Opaque or Mask item (kShadingFlagTransmission), drawn back to front by
    // the translucent forward pass over the transmission copy; they run up to blendDrawItemBegin
    // and are not in the G-buffer.
    size_t transmissiveDrawItemBegin = 0;
    // triangle.frag against the HDR target.
    const VulkanPipelineSet* forwardPipelines = nullptr;
    // KHR_materials_volume_scatter: every draw item whose material scatters, in drawItems' order, and
    // triangle.frag under kScatterPrepass against the scatter pre-pass's images (VulkanScatterPass).
    std::span<const VulkanDrawItem> scatterDrawItems;
    const VulkanPipelineSet* scatterPipelines = nullptr;
    ForwardDrawFilter forwardFilter = ForwardDrawFilter::All;
    // gbuffer.frag against GB0-GB3.
    const VulkanPipelineSet* geometryPipelines = nullptr;
    // The deferred decals (VulkanDrawItem::decal), back to front, which the geometry pass blends
    // into the G-buffer after the opaque surfaces with decalPipelines. In the deferred order they are
    // not among drawItems; the forward-only order has no G-buffer and keeps them there as Blend.
    std::span<const VulkanDrawItem> decalDrawItems;
    const VulkanPipelineSet* decalPipelines = nullptr;
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;
    // The same set as NVRHI's binding set, and the frame's NVRHI command list, whose native command
    // buffer the passes' Record gets: the passes that record through NVRHI (NvrhiPassScope) use both.
    nvrhi::IBindingSet* frameBindingSet = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    // Records large draw lists in parallel (RecordMaterialPass); null records everything inline.
    VulkanParallelRecorder* recorder = nullptr;
    // Physical radiance to HDR target units, the same value as the camera block's exposure.x (see
    // PreExposureFromEv100). Always positive.
    float preExposure = 1.0f;
    // Set 2 for passes that sample the G-buffer; see VulkanGBufferDescriptors.
    VkDescriptorSet gbufferDescriptorSet = VK_NULL_HANDLE;
    // What the tone mapping pass writes to the viewport.
    GBufferDebugView gbufferView = GBufferDebugView::Off;
    // AO parameters for this frame. enabled is already false in the forward-only order.
    AoSettings ao;
    // Which AO history image the resolve reads and writes, and whether the read one is valid.
    TemporalHistoryFrame aoHistory;
    // Whether the TAA pass resolves (on, and the deferred order) or copies the image through, and
    // its history, as aoHistory is the AO resolve's.
    bool taaEnabled = false;
    TemporalHistoryFrame taaHistory;
    // What TAA multiplies its history by (see TaaHistoryScale): 1 without valid history.
    float taaHistoryScale = 1.0f;
    // Set when DLSS replaces the TAA resolve this frame (VulkanTaaPass), with the jitter the frame
    // was rendered with, in render pixels, and whether DLSS throws its history away.
    VulkanDlss* dlss = nullptr;
    glm::vec2 jitterPixels{0.0f};
    bool dlssReset = false;
    float frameTimeMs = 0.0f;
    // The DLSS feature is ray reconstruction (VulkanDlss::HasRayReconstruction): the TAA pass makes its
    // guides and hands it the camera, world to view and the unjittered view to clip.
    bool dlssRayReconstruction = false;
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    BloomSettings bloom;
    // One-bounce indirect diffuse settings; enabled is already false in the forward-only order, and
    // its history as aoHistory is the AO resolve's.
    GiSettings gi;
    TemporalHistoryFrame giHistory;
    // SSR settings; enabled is already false in the forward-only order.
    SsrSettings ssr;
    // Which SSR resolve history image is read and written, and whether the read one is valid.
    TemporalHistoryFrame ssrHistory;
    // The aperture the glare is diffracted through, from this frame's EV (see GlareFNumberFromEv100).
    float glareFNumber = kGlareMinFNumber;
    // Linear Rec.709 to linear Rec.709, applied before tone mapping (see WhiteBalanceMatrix).
    glm::mat3 whiteBalance{1.0f};
    // The swapchain is HDR10: the tone mapping pass uses GT7's HDR curve for hdrPeakNits and writes
    // display-linear values relative to kUiWhiteNits.
    bool hdrOutput = false;
    float hdrPeakNits = 1000.0f;
    // The Khronos reference view: Khronos PBR Neutral instead of GT7's operator, and an HDRI
    // background blurred as the Sample Viewer blurs it.
    bool khronosReference = false;
    // The operator for the shaded image; the Khronos reference view overrides it.
    ToneMapper toneMapper = ToneMapper::Gt7;
    // Increments once per recorded frame; seeds the AO trace's noise.
    uint32_t frameIndex = 0;
    // Passes that trace the ray scene use its ray-query variants: the device supports them and the
    // hardware ray tracing switch is on (RenderDebugSettings::hardwareRayTracing).
    bool hardwareRays = false;
    // The ray traced effects that run this frame: each switch is already false where it cannot
    // (no hardware rays, no ray scene yet, the forward-only order or the Khronos reference view), and
    // probeOcclusion where DDGI is off. The ray scene's sets for this frame slot (VulkanRayScene's ray
    // set and texture table), null when no effect traces.
    RayTracingSettings rayTracing;
    VkDescriptorSet raySet = VK_NULL_HANDLE;
    VkDescriptorSet rayTextureSet = VK_NULL_HANDLE;
    // The traced sun shadow's temporal filter history, as aoHistory is the AO resolve's.
    TemporalHistoryFrame rtShadowHistory;
    // Path tracing in place of the ambient terms (path_trace_pass.h). enabled only where it runs this
    // frame (hardware rays, a ready ray scene, the deferred order, not the Khronos reference view), and
    // then raySet and rayTextureSet are bound; accumulate and denoise are off while DLSS ray
    // reconstruction denoises the raw paths. Its history as aoHistory is the AO resolve's; what that
    // history is multiplied by (this frame's pre-exposure over the one it was written with); and the
    // longest history a pixel may average this frame (PathTraceHistoryCap).
    // With restir set, ReSTIR PT (restir_pt_pass.h) runs instead and the plain path tracer records
    // nothing: whether last frame's reservoirs are this frame's history and which surface buffer is this
    // frame's (writeIndex); last frame's camera position; how many frames its accumulate mode has
    // averaged so far.
    PathTracingSettings pathTracing;
    // The local lights in the scene light list (after its directional ones), for the path tracer's
    // light grid.
    uint32_t localLightCount = 0;
    // The plain path tracer writes its specular hit distance for DLSS ray reconstruction's guides
    // (the specular result's alpha): it runs this frame and ray reconstruction denoises it.
    bool pathTraceHitDistance = false;
    TemporalHistoryFrame pathTraceHistory;
    float pathTraceHistoryScale = 1.0f;
    uint32_t pathTraceHistoryCap = 1;
    TemporalHistoryFrame restirPtHistory;
    glm::vec3 previousCameraPosition{0.0f};
    uint32_t restirPtAccumulatedFrames = 0;
    // The forward-shaded surfaces' layer (path_trace_layer_pass.h), path traced as well this frame
    // (with either path tracer); gbuffer.frag's two layer pipeline sets; and its own history, as
    // pathTraceHistory is the plain path tracer's, and its own accumulate and denoise switches: DLSS
    // ray reconstruction never sees it, so it is always denoised here.
    bool pathTraceLayer = false;
    const VulkanPipelineSet* pathTraceLayerDepthPipelines = nullptr;
    const VulkanPipelineSet* pathTraceLayerSurfacePipelines = nullptr;
    TemporalHistoryFrame pathTraceLayerHistory;
    float pathTraceLayerHistoryScale = 1.0f;
    bool pathTraceLayerAccumulate = false;
    bool pathTraceLayerDenoise = false;
    // The frame's GPU timer, for passes that time their own dispatches (a mark closes the section since
    // the previous one; the renderer marks each pass after it records). Null without timestamps.
    VulkanGpuTimer* gpuTimer = nullptr;
    // The pixels no geometry covered hold the atmosphere or an HDRI: physical radiance that the
    // exposure histogram meters, unlike the flat background of EnvironmentMode::None.
    bool physicalSky = false;
    // The geometry pass draws the atmosphere's ground plane (AtmosphereSettings::groundPlane).
    bool groundPlane = false;
    // The anime character draws (VulkanDrawItem::toon) for the toon passes: the opaque ones, then the
    // transparent ones, each in render queue order. A draw's index here is its toon material's index in
    // VulkanToonMaterials. The opaque ones are among drawItems as well (the geometry pass lays them
    // down); the transparent ones only here.
    std::span<const VulkanDrawItem> toonDrawItems;
    // What the toon passes multiply their radiance by: 2 to the RenderDebugSettings::toonExposureEv.
    float toonExposureScale = 1.0f;
    // The selected entity's submeshes for the selection outline, every one in the frustum but its
    // decals, drawn as the shadow casters are (ShadowDrawItem); empty without a selection. The
    // view-projection is unjittered, so the outline holds still while TAA jitters the scene.
    std::span<const ShadowDrawItem> selectionDrawItems;
    glm::mat4 selectionViewProjection{1.0f};
    // The outline's width, in pixels of the scene targets.
    float selectionOutlineWidth = 2.0f;

    std::span<const VulkanDrawItem> OpaqueDrawItems() const
    {
        return drawItems.first(transmissiveDrawItemBegin);
    }

    // Drawn by the geometry pass like the rest of OpaqueDrawItems, shaded by the forward pass.
    std::span<const VulkanDrawItem> ForwardShadedDrawItems() const
    {
        return drawItems.subspan(forwardShadedDrawItemBegin, transmissiveDrawItemBegin - forwardShadedDrawItemBegin);
    }

    std::span<const VulkanDrawItem> TransmissiveDrawItems() const
    {
        return drawItems.subspan(transmissiveDrawItemBegin, blendDrawItemBegin - transmissiveDrawItemBegin);
    }

    std::span<const VulkanDrawItem> BlendDrawItems() const
    {
        return drawItems.subspan(blendDrawItemBegin);
    }

    // Every draw the forward pass shades: forward-shaded Opaque and Mask, transmissive, Blend.
    std::span<const VulkanDrawItem> PathTraceLayerDrawItems() const
    {
        return drawItems.subspan(forwardShadedDrawItemBegin);
    }
};

// Viewport and scissor are dynamic state on every pipeline in the frame and always cover the
// scene extent. That is what lets a viewport resize rebuild images and framebuffers only.
inline void SetViewportAndScissor(VkCommandBuffer commandBuffer, VkExtent2D extent)
{
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

// One pass in the scene frame. Io() is the declaration the layout tracker turns into barriers;
// Record assumes those barriers have already been issued.
//
// Every implementation's render pass must declare initialLayout == finalLayout for each
// attachment, so that the tracker stays the single authority on layouts. See the note in
// render_target_layout.h.
class IScenePass
{
  public:
    virtual ~IScenePass() = default;

    // Which pass this is. The renderer owns its passes in one list and records them in the order
    // BuildScenePassOrder returns, so the list has to be addressable by id rather than by
    // position: a pass absent from one order is still owned, and still has to follow a rebuilt
    // target set.
    virtual ScenePassId Id() const = 0;
    virtual RenderPassIo Io() const = 0;
    virtual void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const = 0;

    // Called after SceneRenderTargets::Rebuild, so the pass can recreate framebuffers against the
    // new views. Formats never change here, so render passes and pipelines survive.
    virtual void OnTargetsRebuilt(const SceneRenderTargets& targets) = 0;
};
}
