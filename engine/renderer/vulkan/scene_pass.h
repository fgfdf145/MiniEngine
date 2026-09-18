#pragma once

#include "command.h"
#include "common.h"
#include "pipeline_set.h"
#include "render_target_layout.h"
#include "scene_pass_order.h"
#include "scene_render_targets.h"

#include <span>

namespace me
{

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
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;
    // Scale from physical scene radiance to the pre-exposed values the tone mapping operator takes
    // (see ExposureFromEv100). Always positive.
    float exposure = 1.0f;

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
