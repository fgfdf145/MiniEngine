#pragma once

#include "scene_pass.h"

#include <array>
#include <vector>

namespace me
{

// Writes the G-buffer: every Opaque and Mask draw item into GB0-GB3 plus depth. Blend items never
// enter it; the forward pass composites them over the lighting result.
//
// Unlike the forward pass, every attachment including depth is stored, because the lighting pass
// samples all five and the forward blend pass depth-tests against this depth. Framebuffers are
// indexed by frame slot because every attachment is a transient target.
class VulkanGeometryPass : public IScenePass
{
  public:
    // Framebuffer attachment order: the four colors, then depth. gbuffer.frag's output locations
    // 0-3 are the first four entries.
    static constexpr std::array<RenderTargetId, 5> kAttachments = {
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::SceneDepth};
    static constexpr uint32_t kColorAttachmentCount = 4;

    VulkanGeometryPass(VkDevice device, const SceneRenderTargets& targets);
    ~VulkanGeometryPass() override;

    VulkanGeometryPass(const VulkanGeometryPass&) = delete;
    VulkanGeometryPass& operator=(const VulkanGeometryPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The G-buffer material pipelines are built against this. It depends only on the attachment
    // formats, which a resize never changes.
    VkRenderPass GetRenderPass() const;

  private:
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    // Shared by the destructor and the constructor's unwind path, as in every other pass.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
