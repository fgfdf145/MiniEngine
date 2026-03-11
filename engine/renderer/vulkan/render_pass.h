#pragma once

#include "common.h"

class VulkanRenderPass
{
public:
    VulkanRenderPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkFormat swapchainImageFormat,
        VkExtent2D extent,
        const std::vector<VkImageView>& imageViews
    );
    ~VulkanRenderPass();

    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;

    VkRenderPass GetHandle() const;
    const std::vector<VkFramebuffer>& GetFramebuffers() const;

private:
    VkFormat FindDepthFormat() const;
    void CreateDepthResources(VkExtent2D extent);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkFramebuffer> m_framebuffers;
};
