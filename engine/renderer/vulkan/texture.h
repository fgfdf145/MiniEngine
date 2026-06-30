#pragma once

#include "common.h"
#include "upload_batch.h"
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
    // Records this texture's upload (staging copy + layout transitions) into a caller-supplied
    // batch instead of submitting and waiting on its own. The caller must call
    // uploadBatch.Flush() (or otherwise ensure it gets flushed) before the texture is sampled,
    // and keep the batch alive until then.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const std::string& path,
        VulkanUploadBatch& uploadBatch,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor
    );
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const TextureData& textureData,
        VulkanUploadBatch& uploadBatch,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor
    );
    ~VulkanTexture();

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VkImageView GetImageView() const;
    VkSampler GetSampler() const;

private:
    void UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch);
    VkFormat GetVkFormat() const;
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) const;
    void CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) const;
    void TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) const;
    void CopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VulkanTextureFormat m_textureFormat = VulkanTextureFormat::SrgbColor;
};
