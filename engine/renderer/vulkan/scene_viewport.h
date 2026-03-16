#pragma once

#include "common.h"

#include <imgui.h>

#include <vector>

class VulkanSceneViewport
{
public:
    VulkanSceneViewport(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkFormat colorFormat,
        VkExtent2D extent,
        uint32_t frameCount
    );
    ~VulkanSceneViewport();

    VulkanSceneViewport(const VulkanSceneViewport&) = delete;
    VulkanSceneViewport& operator=(const VulkanSceneViewport&) = delete;

    VkRenderPass GetRenderPass() const;
    VkFramebuffer GetFramebuffer(uint32_t frameIndex) const;
    VkExtent2D GetExtent() const;
    ImTextureID GetTextureId(uint32_t frameIndex) const;
    bool MatchesExtent(VkExtent2D extent) const;

private:
    struct FrameResources
    {
        VkImage colorImage = VK_NULL_HANDLE;
        VkDeviceMemory colorMemory = VK_NULL_HANDLE;
        VkImageView colorImageView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthImageView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet textureDescriptorSet = VK_NULL_HANDLE;
    };

    VkFormat FindDepthFormat() const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void CreateRenderPass(VkFormat colorFormat);
    void CreateSampler();
    void CreateFrameResources(uint32_t frameCount);
    void CreateImage(
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImage& image,
        VkDeviceMemory& memory
    ) const;
    VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectMask) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::vector<FrameResources> m_frames;
};
