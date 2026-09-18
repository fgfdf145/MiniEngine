#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// The material pass: every draw item, into the HDR color target with depth. This is
// VulkanSceneViewport's old render pass with two changes — the color attachment is the HDR target
// rather than an sRGB image, and both attachments declare initialLayout == finalLayout so the
// pass performs no implicit transition.
//
// Framebuffers are indexed by frame slot because both attachments are transient targets.
class VulkanForwardPass : public IScenePass
{
  public:
    VulkanForwardPass(VkDevice device, const SceneRenderTargets& targets);
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

    // The material pipelines are built against this. It depends only on the attachment formats,
    // so it survives a resize and a swapchain recreate.
    VkRenderPass GetRenderPass() const;

  private:
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    // Shared by the destructor and the constructor's unwind path, the way VulkanTonemapPass does
    // it: a throw part way through construction skips the destructor, so both need the same
    // teardown and keeping one list of it is what stops the two drifting apart.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
