#include "texture.h"

#include <cstring>

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue,
    const std::string& path,
    VulkanTextureFormat textureFormat
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_graphicsQueueFamily(graphicsQueueFamily),
      m_graphicsQueue(graphicsQueue),
      m_textureFormat(textureFormat)
{
    UploadTexture(TextureLoader::LoadRGBA8(path));
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue,
    const TextureData& textureData,
    VulkanTextureFormat textureFormat
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_graphicsQueueFamily(graphicsQueueFamily),
      m_graphicsQueue(graphicsQueue),
      m_textureFormat(textureFormat)
{
    UploadTexture(textureData);
}

void VulkanTexture::UploadTexture(const TextureData& textureData)
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

    CreateImage(
        static_cast<uint32_t>(textureData.width),
        static_cast<uint32_t>(textureData.height),
        vkFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        m_image,
        m_memory
    );

    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(stagingBuffer, m_image, static_cast<uint32_t>(textureData.width), static_cast<uint32_t>(textureData.height));
    TransitionImageLayout(m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkFreeMemory(m_device, stagingMemory, nullptr);
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView), "Failed to create texture image view");

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
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
    imageInfo.mipLevels = 1;
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

void VulkanTexture::TransitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) const
{
    VkCommandPool commandPool = VK_NULL_HANDLE;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &commandPool), "Failed to create texture command pool");

    const VkCommandBuffer commandBuffer = BeginSingleUseCommandBuffer(commandPool);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
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
        vkDestroyCommandPool(m_device, commandPool, nullptr);
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

    EndSingleUseCommandBuffer(commandPool, commandBuffer);
    vkDestroyCommandPool(m_device, commandPool, nullptr);
}

void VulkanTexture::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const
{
    VkCommandPool commandPool = VK_NULL_HANDLE;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &commandPool), "Failed to create texture copy command pool");

    const VkCommandBuffer commandBuffer = BeginSingleUseCommandBuffer(commandPool);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndSingleUseCommandBuffer(commandPool, commandBuffer);
    vkDestroyCommandPool(m_device, commandPool, nullptr);
}

VkCommandBuffer VulkanTexture::BeginSingleUseCommandBuffer(VkCommandPool commandPool) const
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer), "Failed to allocate texture command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin texture command buffer");

    return commandBuffer;
}

void VulkanTexture::EndSingleUseCommandBuffer(VkCommandPool commandPool, VkCommandBuffer commandBuffer) const
{
    CheckVulkan(vkEndCommandBuffer(commandBuffer), "Failed to end texture command buffer");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    CheckVulkan(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit texture command buffer");
    CheckVulkan(vkQueueWaitIdle(m_graphicsQueue), "Failed waiting for texture command buffer");

    vkFreeCommandBuffers(m_device, commandPool, 1, &commandBuffer);
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
