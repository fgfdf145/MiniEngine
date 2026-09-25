#pragma once

#include "scene_pass.h"

#include <array>
#include <vector>

namespace me
{

// Writes the G-buffer: every Opaque and Mask draw item into GB0-GB3, the motion vectors and depth.
// Blend items never enter it; the forward pass composites them over the lighting result.
//
// Unlike the forward pass, every attachment including depth is stored, because later passes
// sample them and the forward blend pass depth-tests against this depth. Framebuffers are
// indexed by frame slot because every attachment is a transient target. The velocity clear value
// is zero, which is also what a pixel no geometry covered reads: no motion vector.
class VulkanGeometryPass : public IScenePass
{
  public:
    // Framebuffer attachment order: the six colors, then depth. gbuffer.frag's output locations
    // 0-5 are the first six entries.
    static constexpr std::array<RenderTargetId, 9> kAttachments = {
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferSheen,
        RenderTargetId::SceneDepth};
    // The most MoltenVK offers on Apple GPUs; VulkanDevice refuses a device with fewer.
    static constexpr uint32_t kColorAttachmentCount = 8;

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
