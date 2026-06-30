#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const std::string& path,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(textureFormat)
{
    UploadTexture(TextureLoader::LoadRGBA8(path), uploadBatch);
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const TextureData& textureData,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(textureFormat)
{
    UploadTexture(textureData, uploadBatch);
}

void VulkanTexture::UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch)
{
    if (!textureData.IsValid())
    {
        throw std::runtime_error("Cannot create Vulkan texture from invalid pixel data");
    }

    const VkDeviceSize imageSize =
        static_cast<VkDeviceSize>(textureData.width) *
        static_cast<VkDeviceSize>(textureData.height) *
        4;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    const VkFormat vkFormat = GetVkFormat();
    CreateBuffer(
        imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory
    );

    void* mappedData = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mappedData), "Failed to map texture staging buffer");
    std::memcpy(mappedData, textureData.pixels.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);

    const bool canGenerateMips = FormatSupportsLinearBlit(vkFormat);
    m_mipLevels = canGenerateMips
        ? static_cast<uint32_t>(std::floor(std::log2(
              static_cast<double>(std::max(textureData.width, textureData.height))
          ))) + 1
        : 1;

    CreateImage(
        static_cast<uint32_t>(textureData.width),
        static_cast<uint32_t>(textureData.height),
        m_mipLevels,
        vkFormat,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        m_image,
        m_memory
    );

    // All commands below go into the caller's shared batch command buffer, in this same order,
    // so the transition -> copy -> mip-chain sequence for this image is preserved exactly as
    // before even though many other textures' sequences are interleaved in the same command
    // buffer. Each barrier only touches this image, so that's safe.
    const VkCommandBuffer commandBuffer = uploadBatch.GetCommandBuffer();
    // Transition every mip level to TRANSFER_DST_OPTIMAL up front: the copy below only fills
    // level 0, but GenerateMipmaps()'s blit chain expects every level to already be in that
    // layout (it reads each source level back out of TRANSFER_DST_OPTIMAL).
    TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, m_mipLevels);
    CopyBufferToImage(commandBuffer, stagingBuffer, m_image, static_cast<uint32_t>(textureData.width), static_cast<uint32_t>(textureData.height));

    if (m_mipLevels > 1)
    {
        GenerateMipmaps(commandBuffer, m_image, textureData.width, textureData.height, m_mipLevels);
    }
    else
    {
        TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView), "Failed to create texture image view");

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);
    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &physicalDeviceProperties);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = supportedFeatures.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = supportedFeatures.samplerAnisotropy
        ? std::min(16.0f, physicalDeviceProperties.limits.maxSamplerAnisotropy)
        : 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_mipLevels);
    samplerInfo.mipLodBias = 0.0f;
    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create texture sampler");
}

VkFormat VulkanTexture::GetVkFormat() const
{
    return m_textureFormat == VulkanTextureFormat::SrgbColor
        ? VK_FORMAT_R8G8B8A8_SRGB
        : VK_FORMAT_R8G8B8A8_UNORM;
}

VulkanTexture::~VulkanTexture()
{
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
    if (m_imageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_imageView, nullptr);
    }
    if (m_image != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_image, nullptr);
    }
    if (m_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_memory, nullptr);
    }
}

VkImageView VulkanTexture::GetImageView() const
{
    return m_imageView;
}

VkSampler VulkanTexture::GetSampler() const
{
    return m_sampler;
}

void VulkanTexture::CreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory
) const
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create texture buffer");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, properties);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate texture buffer memory");
    CheckVulkan(vkBindBufferMemory(m_device, buffer, memory, 0), "Failed to bind texture buffer memory");
}

void VulkanTexture::CreateImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory
) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &image), "Failed to create texture image");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(m_device, image, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate texture image memory");
    CheckVulkan(vkBindImageMemory(m_device, image, memory, 0), "Failed to bind texture image memory");
}

void VulkanTexture::TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t baseMipLevel, uint32_t levelCount) const
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        throw std::runtime_error("Unsupported texture image layout transition");
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );
}

void VulkanTexture::CopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const
{
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

bool VulkanTexture::FormatSupportsLinearBlit(VkFormat format) const
{
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);

    constexpr VkFormatFeatureFlags kRequiredFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (formatProperties.optimalTilingFeatures & kRequiredFeatures) == kRequiredFeatures;
}

// Blits each mip level down from the previous one, halving width/height each step. Every level
// must already be in TRANSFER_DST_OPTIMAL when this is called (see UploadTexture's up-front,
// whole-mip-range transition); each iteration reuses level (i - 1) as the blit source by flipping
// it to TRANSFER_SRC_OPTIMAL, then leaves it in SHADER_READ_ONLY_OPTIMAL once it's done being
// read from. The final (smallest) level is transitioned to SHADER_READ_ONLY_OPTIMAL after the loop
// since it's only ever a blit destination.
void VulkanTexture::GenerateMipmaps(VkCommandBuffer commandBuffer, VkImage image, int32_t texWidth, int32_t texHeight, uint32_t mipLevels) const
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t level = 1; level < mipLevels; ++level)
    {
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier
        );

        const int32_t nextMipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        const int32_t nextMipHeight = mipHeight > 1 ? mipHeight / 2 : 1;

        VkImageBlit blit{};
        blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[1] = { nextMipWidth, nextMipHeight, 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(
            commandBuffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR
        );

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier
        );

        mipWidth = nextMipWidth;
        mipHeight = nextMipHeight;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );
}

uint32_t VulkanTexture::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertiesMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertiesMatch)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable texture memory type");
}
