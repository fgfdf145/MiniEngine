#include "transmission_copy.h"

#include <array>

namespace me
{

namespace
{
constexpr VkFormat kCopyFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match TransmissionCopyConstants in shaders/vulkan/transmission_copy.comp.
struct TransmissionCopyConstants
{
    glm::uvec2 extent{0u};
};

VkImageMemoryBarrier LevelBarrier(
    VkImage image,
    uint32_t baseLevel,
    uint32_t levelCount,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkAccessFlags srcAccess,
    VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levelCount, 0, 1};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    return barrier;
}

void RecordBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags src, VkPipelineStageFlags dst, const VkImageMemoryBarrier& barrier)
{
    vkCmdPipelineBarrier(commandBuffer, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
}

VulkanTransmissionImage::VulkanTransmissionImage(VkPhysicalDevice physicalDevice, VkDevice device)
    : m_device(device)
{
    try
    {
        // The mip chain is built with linear blits, which the format must support.
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, kCopyFormat, &properties);
        constexpr VkFormatFeatureFlags kRequired = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                                   VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & kRequired) != kRequired)
        {
            throw std::runtime_error("RGBA16F cannot be blitted, filtered and stored on this device; transmission needs it");
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {kSize, kSize, 1};
        imageInfo.mipLevels = kMipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kCopyFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &m_image), "Failed to create the transmission copy");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, m_image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_memory), "Failed to allocate the transmission copy");
        CheckVulkan(vkBindImageMemory(m_device, m_image, m_memory, 0), "Failed to bind the transmission copy");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kCopyFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipLevels, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_view), "Failed to create the transmission copy view");
        viewInfo.subresourceRange.levelCount = 1;
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_level0View), "Failed to create the transmission copy's level view");

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(kMipLevels);
        CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create the transmission copy's sampler");
    }
    catch (...)
    {
        Destroy();
        throw;
    }
}

VulkanTransmissionImage::~VulkanTransmissionImage()
{
    Destroy();
}

TextureDescriptorBinding VulkanTransmissionImage::GetSampledBinding() const
{
    return TextureDescriptorBinding{m_view, m_sampler};
}

VkImage VulkanTransmissionImage::GetImage() const
{
    return m_image;
}

VkImageView VulkanTransmissionImage::GetLevel0View() const
{
    return m_level0View;
}

void VulkanTransmissionImage::RecordInitialTransition(VkCommandBuffer commandBuffer) const
{
    if (m_initialized)
    {
        return;
    }
    RecordBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        LevelBarrier(m_image, 0, kMipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT));
    m_initialized = true;
}

void VulkanTransmissionImage::Destroy()
{
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    for (VkImageView* view : {&m_level0View, &m_view})
    {
        if (*view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, *view, nullptr);
            *view = VK_NULL_HANDLE;
        }
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

VulkanTransmissionCopyPass::VulkanTransmissionCopyPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanTransmissionImage& image)
    : m_device(device),
      m_image(image)
{
    try
    {
        m_sampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 2> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(
            m_device,
            pipelineCache,
            frameSetLayout,
            m_setLayout,
            "transmission_copy.comp.spv",
            sizeof(TransmissionCopyConstants),
            m_pipelineLayout,
            m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 1, 1);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTransmissionCopyPass::~VulkanTransmissionCopyPass()
{
    DestroyHandles();
}

ScenePassId VulkanTransmissionCopyPass::Id() const
{
    return ScenePassId::TransmissionCopy;
}

RenderPassIo VulkanTransmissionCopyPass::Io() const
{
    // Sampled: the tracker puts the HDR target in SHADER_READ_ONLY_OPTIMAL after the forward pass's
    // writes, and the translucent pass's write declaration takes it back.
    static constexpr std::array<RenderTargetId, 1> kReads = {RenderTargetId::SceneHdr};
    RenderPassIo io{};
    io.reads = kReads;
    return io;
}

void VulkanTransmissionCopyPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    m_image.RecordInitialTransition(commandBuffer);
    if (frame.TransmissiveDrawItems().empty())
    {
        return;
    }

    const VkImage image = m_image.GetImage();
    constexpr uint32_t kLevels = VulkanTransmissionImage::kMipLevels;
    // Whatever the copy held is discarded; the last frame's reads of it are done before the writes.
    RecordBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        LevelBarrier(image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT));
    RecordBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        LevelBarrier(image, 1, kLevels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    TransmissionCopyConstants constants{};
    constants.extent = glm::uvec2(VulkanTransmissionImage::kSize);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_sets.at(slot),
        &constants,
        sizeof(constants),
        VkExtent2D{VulkanTransmissionImage::kSize, VulkanTransmissionImage::kSize});

    // Level 0 becomes the first blit's source; each level after it is blitted from the one above.
    RecordBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        LevelBarrier(image, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
    int32_t size = static_cast<int32_t>(VulkanTransmissionImage::kSize);
    for (uint32_t level = 1; level < kLevels; ++level)
    {
        const int32_t next = std::max(size / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {size, size, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {next, next, 1};
        vkCmdBlitImage(
            commandBuffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_LINEAR);
        RecordBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            LevelBarrier(image, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        size = next;
    }
    // Back to rest for the translucent pass's fragment shaders.
    RecordBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        LevelBarrier(image, 0, kLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
}

void VulkanTransmissionCopyPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanTransmissionCopyPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorImageInfo sourceInfo{m_sampler, targets.GetView(RenderTargetId::SceneHdr, slot), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo destinationInfo{VK_NULL_HANDLE, m_image.GetLevel0View(), VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 2> writes = {
            ImageWrite(m_sets[slot], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourceInfo),
            ImageWrite(m_sets[slot], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destinationInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanTransmissionCopyPass::DestroyHandles()
{
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}
}
