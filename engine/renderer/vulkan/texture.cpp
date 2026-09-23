#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace me
{

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const std::string& path,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(textureFormat)
{
    try
    {
        UploadTexture(TextureLoader::LoadRGBA8(path), uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const TextureData& textureData,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(textureFormat)
{
    // A throw out of a constructor skips the destructor, so whatever was created before the
    // failure is released here with the same call the destructor makes.
    try
    {
        UploadTexture(textureData, uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const HalfFloatTextureData& textureData,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!textureData.IsValid())
        {
            throw std::runtime_error("Cannot create Vulkan texture from invalid half-float data");
        }
        UploadTexels(
            textureData.texels.data(),
            static_cast<VkDeviceSize>(textureData.texels.size() * sizeof(std::uint16_t)),
            static_cast<uint32_t>(textureData.width),
            static_cast<uint32_t>(textureData.height),
            VK_FORMAT_R16G16B16A16_SFLOAT,
            uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const FloatTextureData& equirectangular,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!equirectangular.IsValid())
        {
            throw std::runtime_error("Cannot create an environment map from invalid float data");
        }
        const uint32_t width = static_cast<uint32_t>(equirectangular.width);
        const uint32_t height = static_cast<uint32_t>(equirectangular.height);
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0)
        {
            UploadTexels(
                equirectangular.pixels.data(),
                static_cast<VkDeviceSize>(equirectangular.pixels.size() * sizeof(float)),
                width,
                height,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                uploadBatch,
                false,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
        else
        {
            const HalfFloatTextureData packed = PackRgba16Float(equirectangular);
            UploadTexels(
                packed.texels.data(),
                static_cast<VkDeviceSize>(packed.texels.size() * sizeof(std::uint16_t)),
                width,
                height,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                uploadBatch,
                false,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const CompressedTexture& texture,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        UploadCompressedTexture(texture, uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

void VulkanTexture::UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch)
{
    if (!textureData.IsValid())
    {
        throw std::runtime_error("Cannot create Vulkan texture from invalid pixel data");
    }

    UploadTexels(
        textureData.pixels.data(),
        static_cast<VkDeviceSize>(textureData.width) * static_cast<VkDeviceSize>(textureData.height) * 4,
        static_cast<uint32_t>(textureData.width),
        static_cast<uint32_t>(textureData.height),
        GetVkFormat(),
        uploadBatch);
}

void VulkanTexture::UploadTexels(
    const void* texels,
    VkDeviceSize byteCount,
    uint32_t width,
    uint32_t height,
    VkFormat vkFormat,
    VulkanUploadBatch& uploadBatch,
    bool generateMips,
    VkSamplerAddressMode addressModeV)
{
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        byteCount,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory);
    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

    void* mappedData = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, byteCount, 0, &mappedData), "Failed to map texture staging buffer");
    std::memcpy(mappedData, texels, static_cast<size_t>(byteCount));
    vkUnmapMemory(m_device, stagingMemory);

    const bool canGenerateMips = FormatSupportsLinearBlit(vkFormat);
    m_mipLevels = canGenerateMips && generateMips
                      ? static_cast<uint32_t>(std::floor(std::log2(
                            static_cast<double>(std::max(width, height))))) +
                            1
                      : 1;

    CreateImage(
        width,
        height,
        m_mipLevels,
        vkFormat,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        m_image,
        m_memory);

    // All commands below go into the caller's shared batch command buffer, in this same order,
    // so the transition -> copy -> mip-chain sequence for this image is preserved exactly as
    // before even though many other textures' sequences are interleaved in the same command
    // buffer. Each barrier only touches this image, so that's safe.
    const VkCommandBuffer commandBuffer = uploadBatch.GetCommandBuffer();
    // Transition every mip level to TRANSFER_DST_OPTIMAL up front: the copy below only fills
    // level 0, but GenerateMipmaps()'s blit chain expects every level to already be in that
    // layout (it reads each source level back out of TRANSFER_DST_OPTIMAL).
    TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, m_mipLevels);
    CopyBufferToImage(commandBuffer, stagingBuffer, m_image, width, height);

    if (m_mipLevels > 1)
    {
        GenerateMipmaps(commandBuffer, m_image, static_cast<int32_t>(width), static_cast<int32_t>(height), m_mipLevels);
    }
    else
    {
        TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    CreateViewAndSampler(vkFormat, addressModeV);
}

void VulkanTexture::UploadCompressedTexture(const CompressedTexture& texture, VulkanUploadBatch& uploadBatch)
{
    if (texture.levels.empty())
    {
        throw std::runtime_error("Cannot create a Vulkan texture from a compressed texture with no levels");
    }

    // Every level goes into one staging buffer, back to back. Each level is a whole number of
    // 16-byte blocks, so every level's offset meets the block-size alignment copies require.
    VkDeviceSize totalSize = 0;
    for (const CompressedTextureLevel& level : texture.levels)
    {
        totalSize += static_cast<VkDeviceSize>(level.blocks.size());
    }

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        totalSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory);
    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(texture.levels.size());
    void* mappedData = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, totalSize, 0, &mappedData), "Failed to map compressed texture staging buffer");
    VkDeviceSize offset = 0;
    for (uint32_t levelIndex = 0; levelIndex < static_cast<uint32_t>(texture.levels.size()); ++levelIndex)
    {
        const CompressedTextureLevel& level = texture.levels[levelIndex];
        std::memcpy(static_cast<uint8_t*>(mappedData) + offset, level.blocks.data(), level.blocks.size());

        // The extent is the level's pixel size, which a partial edge block may exceed; Vulkan
        // accepts that for block formats when the extent reaches the edge of the level.
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, levelIndex, 0, 1};
        region.imageExtent = {level.width, level.height, 1};
        regions.push_back(region);
        offset += static_cast<VkDeviceSize>(level.blocks.size());
    }
    vkUnmapMemory(m_device, stagingMemory);

    const VkFormat vkFormat = ToVkFormat(texture.format);
    m_mipLevels = static_cast<uint32_t>(texture.levels.size());
    CreateImage(
        texture.levels[0].width,
        texture.levels[0].height,
        m_mipLevels,
        vkFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        m_image,
        m_memory);

    // The whole chain arrives in one copy: no blits, which block formats could not do anyway.
    const VkCommandBuffer commandBuffer = uploadBatch.GetCommandBuffer();
    TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, m_mipLevels);
    vkCmdCopyBufferToImage(
        commandBuffer,
        stagingBuffer,
        m_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()),
        regions.data());
    TransitionImageLayout(commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, m_mipLevels);

    CreateViewAndSampler(vkFormat);
}

VkFormat VulkanTexture::ToVkFormat(CompressedTextureFormat format)
{
    switch (format)
    {
    case CompressedTextureFormat::Bc7Srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case CompressedTextureFormat::Bc7Unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case CompressedTextureFormat::Bc5Unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    }
    throw std::runtime_error("Unknown compressed texture format");
}

void VulkanTexture::CreateViewAndSampler(VkFormat vkFormat, VkSamplerAddressMode addressModeV)
{
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
    samplerInfo.addressModeV = addressModeV;
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
    DestroyHandles();
}

void VulkanTexture::DestroyHandles()
{
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_imageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
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
    VkDeviceMemory& memory) const
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create texture buffer");

    // Either both handles come back valid or neither does: the caller only takes ownership of a
    // complete staging buffer.
    try
    {
        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, properties);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate texture buffer memory");
        CheckVulkan(vkBindBufferMemory(m_device, buffer, memory, 0), "Failed to bind texture buffer memory");
    }
    catch (...)
    {
        vkFreeMemory(m_device, memory, nullptr);
        memory = VK_NULL_HANDLE;
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        throw;
    }
}

void VulkanTexture::CreateImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory) const
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
        &barrier);
}

void VulkanTexture::CopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const
{
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};

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
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        const int32_t nextMipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        const int32_t nextMipHeight = mipHeight > 1 ? mipHeight / 2 : 1;

        VkImageBlit blit{};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[1] = {nextMipWidth, nextMipHeight, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(
            commandBuffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

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
        0, 0, nullptr, 0, nullptr, 1, &barrier);
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
}
