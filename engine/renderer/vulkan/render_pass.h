#pragma once

#include "common.h"

class VulkanRenderPass
{
public:
    VulkanRenderPass(VkDevice device, VkFormat swapchainImageFormat, VkExtent2D extent, const std::vector<VkImageView>& imageViews);
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
