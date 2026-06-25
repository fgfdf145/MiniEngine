#pragma once

#include "common.h"
#include <texture_loader.h>

#include <string>

enum class VulkanTextureFormat
{
    SrgbColor,
    LinearData
};

class VulkanTexture
{
public:
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue,
        const std::string& path,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor
    );
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue,
        const TextureData& textureData,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor
    );
    ~VulkanTexture();

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VkImageView GetImageView() const;
    VkSampler GetSampler() const;

private:
    void UploadTexture(const TextureData& textureData);
    VkFormat GetVkFormat() const;
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) const;
    void CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) const;
    void TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) const;
    void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const;
    VkCommandBuffer BeginSingleUseCommandBuffer(VkCommandPool commandPool) const;
    void EndSingleUseCommandBuffer(VkCommandPool commandPool, VkCommandBuffer commandBuffer) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VulkanTextureFormat m_textureFormat = VulkanTextureFormat::SrgbColor;
};
