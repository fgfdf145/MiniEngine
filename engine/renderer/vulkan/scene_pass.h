#pragma once

#include "command.h"
#include "common.h"
#include "pipeline_set.h"
#include "render_target_layout.h"
#include "scene_pass_order.h"
#include "scene_render_targets.h"

#include <engine/renderer/temporal_history.h>
#include <engine/renderer/exposure.h>
#include <engine/renderer/glare.h>
#include <engine/renderer/render_types.h>

#include <span>

namespace me
{

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
    VkExtent2D extent{};
    // Every draw item for the frame: Opaque and Mask first in pipeline variant order, then Blend
    // back to front, the partition BuildMaterialDrawOrder produces. blendDrawItemBegin is the
    // index of the first Blend item, so the two halves are views of one vector, never copies.
    std::span<const VulkanDrawItem> drawItems;
    size_t blendDrawItemBegin = 0;
    // triangle.frag against the HDR target.
    const VulkanPipelineSet* forwardPipelines = nullptr;
    ForwardDrawFilter forwardFilter = ForwardDrawFilter::All;
    // gbuffer.frag against GB0-GB3.
    const VulkanPipelineSet* geometryPipelines = nullptr;
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;
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
    BloomSettings bloom;
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
    // Increments once per recorded frame; seeds the AO trace's noise.
    uint32_t frameIndex = 0;
    // The pixels no geometry covered hold the atmosphere or an HDRI: physical radiance that the
    // exposure histogram meters, unlike the flat background of EnvironmentMode::None.
    bool physicalSky = false;

    std::span<const VulkanDrawItem> OpaqueDrawItems() const
    {
        return drawItems.first(blendDrawItemBegin);
    }

    std::span<const VulkanDrawItem> BlendDrawItems() const
    {
        return drawItems.subspan(blendDrawItemBegin);
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
