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
    std::span<const VulkanDrawItem> drawItems;
    const VulkanPipelineSet* pipelines = nullptr;
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;
    // Scale from physical scene radiance to the pre-exposed values the tone mapping operator takes
    // (see ExposureFromEv100). Always positive.
    float exposure = 1.0f;
};

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
