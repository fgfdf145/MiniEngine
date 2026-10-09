#pragma once

#include "common.h"
#include "memory_pool.h"
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

// An image (and on Vulkan its native view), without a sampler: callers pair it with one from
// VulkanSamplerCache, so a scene with thousands of textures stays far below the device's sampler
// limit. The image is NVRHI's, bound to a range of VulkanMemoryPool's heaps, uploaded through the
// batch's NVRHI writes, and rests as a shader resource (keepInitialState).
class VulkanTexture
{
  public:
    // Records this texture's upload into a caller-supplied batch instead of submitting and waiting
    // on its own. The caller submits the batch before the texture is sampled.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const std::string& path,
        VulkanUploadBatch& uploadBatch,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor);
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const TextureData& textureData,
        VulkanUploadBatch& uploadBatch,
        VulkanTextureFormat textureFormat = VulkanTextureFormat::SrgbColor);
    // Uploads a linear RGBA16F image as R16G16B16A16_SFLOAT with its mips. The format has no
    // sRGB variant and needs none: float images are scene-linear whatever slot samples them.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const HalfFloatTextureData& textureData,
        VulkanUploadBatch& uploadBatch);
    // An equirectangular environment map: R32G32B32A32_SFLOAT when the device filters that format
    // linearly, else packed to R16G16B16A16_SFLOAT (values clamp at 65504). One mip level: the map
    // is magnified, never minified, and a mip chain would seam where the longitude wraps. Sample it
    // with a sampler that repeats in u and clamps in v.
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const FloatTextureData& equirectangular,
        VulkanUploadBatch& uploadBatch);
    // Uploads a block-compressed texture with its whole mip chain, as prepared by
    // CompressTexture. The device must support block compression (see
    // VulkanDevice::SupportsBlockCompression).
    VulkanTexture(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const CompressedTexture& texture,
        VulkanUploadBatch& uploadBatch);
    ~VulkanTexture();

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    VkImageView GetImageView() const;
    nvrhi::ITexture* GetNvrhiTexture() const;

  private:
    // Shared by the destructor and the constructors' unwind path. Skips null handles.
    void DestroyHandles();
    // Uploads an RGBA8 image in this texture's sRGB or linear format.
    void UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch);
    // Uploads level 0 from tightly packed texels (RGBA8, RGBA16F or RGBA32F) and, with generateMips,
    // the mip chain built from it on the CPU (not for RGBA32F).
    void UploadTexels(const void* texels, uint32_t width, uint32_t height, VkFormat vkFormat, VulkanUploadBatch& uploadBatch, bool generateMips = true);
    void UploadCompressedTexture(const CompressedTexture& texture, VulkanUploadBatch& uploadBatch);
    // The native view, on Vulkan, once the image holds every level.
    void CreateView(VkFormat vkFormat);
    static VkFormat ToVkFormat(CompressedTextureFormat format);
    VkFormat GetVkFormat() const;
    // A sampled 2D image of format with mipLevels levels into m_texture, m_image and m_memory.
    void CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::TextureHandle m_texture;
    VkImage m_image = VK_NULL_HANDLE;
    VulkanPooledMemory m_memory;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VulkanTextureFormat m_textureFormat = VulkanTextureFormat::SrgbColor;
    uint32_t m_mipLevels = 1;
};
}
