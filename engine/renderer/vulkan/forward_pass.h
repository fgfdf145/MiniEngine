#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// The material pass into the HDR target with depth. In the forward-only comparison order it owns
// the frame and draws every item; in the deferred order it draws only Blend items over the lighting
// pass's result. Either way it draws the sky between its opaque and blend items, into the pixels
// no geometry covered. Both attachments declare initialLayout == finalLayout so the pass performs no
// implicit transition.
//
// Framebuffers are indexed by frame slot because both attachments are transient targets.
class VulkanForwardPass : public IScenePass
{
  public:
    VulkanForwardPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanForwardPass() override;

    VulkanForwardPass(const VulkanForwardPass&) = delete;
    VulkanForwardPass& operator=(const VulkanForwardPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The material pipelines are built against this. Either variant would do, since they are
    // compatible; this returns the clear one. It depends only on the attachment formats, so it
    // survives a resize and a swapchain recreate.
    VkRenderPass GetRenderPass() const;

  private:
    VkRenderPass CreateRenderPass(const SceneRenderTargets& targets, VkAttachmentLoadOp loadOp) const;
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void RecordSky(VkCommandBuffer commandBuffer, const ScenePassFrameContext& frame) const;
    void DestroyFramebuffers();
    // Shared by the destructor and the constructor's unwind path, the way VulkanTonemapPass does
    // it: a throw part way through construction skips the destructor, so both need the same
    // teardown and keeping one list of it is what stops the two drifting apart.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    // Identical except for both attachments' loadOp: CLEAR when the pass owns the frame, LOAD when
    // it composites over the deferred result. Compatible with each other, so the framebuffers are
    // created against the clear variant and serve both.
    VkRenderPass m_clearRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_loadRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    // sky.vert and sky.frag: set 0 plus a 16-byte push constant, the background radiance.
    VkPipelineLayout m_skyPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_skyPipeline = VK_NULL_HANDLE;
};
}
