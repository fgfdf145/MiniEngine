#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// Which half of the forward drawing a VulkanForwardPass records. The transmission copy runs
// between them, so transmissive surfaces see everything the first half drew.
enum class ForwardPassPart
{
    // The Opaque and Mask items it shades (all of them when it owns the frame, the forward-shaded
    // ones otherwise), then the sky into the pixels no geometry covered.
    OpaqueAndSky,
    // Transmissive items back to front, then Blend items back to front, over what is there.
    Translucent
};

// The material pass into the HDR target with depth. In the forward-only comparison order its
// opaque half owns the frame and draws every opaque item; in the deferred order it draws only the
// forward-shaded ones over the lighting pass's result. Both attachments declare initialLayout ==
// finalLayout so the pass performs no implicit transition.
//
// Framebuffers are indexed by frame slot because both attachments are transient targets.
class VulkanForwardPass : public IScenePass
{
  public:
    VulkanForwardPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        ForwardPassPart part);
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
    ForwardPassPart m_part = ForwardPassPart::OpaqueAndSky;
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
