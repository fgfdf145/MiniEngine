#pragma once

#include "common.h"

namespace me
{

// The swapchain render pass. It draws only the editor's ImGui layer, which renders with depth
// testing disabled, so this pass has a single color attachment — no depth image, no per-frame
// depth clear, and no cross-frame write-after-write on a shared depth buffer. The 3D scene has
// its own depth buffer in VulkanSceneViewport.
class VulkanRenderPass
{
  public:
    VulkanRenderPass(
        VkDevice device,
        VkFormat swapchainImageFormat,
        VkExtent2D extent,
        const std::vector<VkImageView>& imageViews);
    ~VulkanRenderPass();

    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;

    VkRenderPass GetHandle() const;
    const std::vector<VkFramebuffer>& GetFramebuffers() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
