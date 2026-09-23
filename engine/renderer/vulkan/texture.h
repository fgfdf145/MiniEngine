#pragma once

#include "common.h"
#include "upload_batch.h"
#include <engine/asset/texture_compression.h>
#include <engine/asset/texture_loader.h>

#include <string>

namespace me
{

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
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor);
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const TextureData& textureData,
        VulkanUploadBatch& uploadBatch,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor);
    // Uploads a linear RGBA16F image as R16G16B16A16_SFLOAT with GPU-built mips. The format has no
    // sRGB variant and needs none: float images are scene-linear whatever slot samples them.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const HalfFloatTextureData& textureData,
        VulkanUploadBatch& uploadBatch);
    // An equirectangular environment map: R32G32B32A32_SFLOAT when the device filters that format
    // linearly, else packed to R16G16B16A16_SFLOAT (values clamp at 65504). One mip level: the map
    // is magnified, never minified, and a mip chain would seam where the longitude wraps. Repeats
    // in u, clamps in v.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const FloatTextureData& equirectangular,
        VulkanUploadBatch& uploadBatch);
    // Uploads a block-compressed texture with its whole mip chain, as prepared by
    // CompressTexture. The device must support block compression (see
    // VulkanDevice::SupportsBlockCompression).
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const CompressedTexture& texture,
        VulkanUploadBatch& uploadBatch);
    ~VulkanTexture();

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VkImageView GetImageView() const;
    VkSampler GetSampler() const;

  private:
    // Shared by the destructor and the constructors' unwind path. Skips null handles.
    void DestroyHandles();
    // Uploads an RGBA8 image in this texture's sRGB or linear format.
    void UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch);
    // Uploads level 0 from tightly packed texels and builds the mip chain with linear blits when the
    // format supports them, else keeps a single level. Shared by the RGBA8 and half-float paths.
    void UploadTexels(const void* texels, VkDeviceSize byteCount, uint32_t width, uint32_t height, VkFormat vkFormat, VulkanUploadBatch& uploadBatch, bool generateMips = true, VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT);
    void UploadCompressedTexture(const CompressedTexture& texture, VulkanUploadBatch& uploadBatch);
    // Shared by both upload paths once the image holds every level in shader read layout.
    void CreateViewAndSampler(VkFormat vkFormat, VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT);
    static VkFormat ToVkFormat(CompressedTextureFormat format);
    VkFormat GetVkFormat() const;
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) const;
    void CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory) const;
    void TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t baseMipLevel, uint32_t levelCount) const;
    void CopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const;
    void GenerateMipmaps(VkCommandBuffer commandBuffer, VkImage image, int32_t texWidth, int32_t texHeight, uint32_t mipLevels) const;
    bool FormatSupportsLinearBlit(VkFormat format) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VulkanTextureFormat m_textureFormat = VulkanTextureFormat::SrgbColor;
    uint32_t m_mipLevels = 1;
};
}
