#include "transmission_copy.h"

#include "nvrhi_pass.h"
#include "nvrhi_resources.h"
#include "sampler_settings.h"

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

VulkanTransmissionImage::VulkanTransmissionImage(VkPhysicalDevice physicalDevice, VkDevice device, nvrhi::IDevice* nvrhiDevice)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice)
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
        m_texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, m_image, "Failed to create the transmission copy");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kCopyFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMipLevels, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_view), "Failed to create the transmission copy view");

        nvrhi::SamplerDesc samplerDesc = BuildClampSamplerDesc(true);
        samplerDesc.mipFilter = true;
        samplerDesc.maxLod = static_cast<float>(kMipLevels);
        m_sampler = CreateNvrhiSampler(nvrhiDevice, samplerDesc, "Failed to create the transmission copy's sampler");
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
    return BindTexture(m_view, m_texture, m_sampler);
}

VkImage VulkanTransmissionImage::GetImage() const
{
    return m_image;
}

nvrhi::ITexture* VulkanTransmissionImage::GetTexture() const
{
    return m_texture;
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
    m_sampler = nullptr;
    if (m_view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }
    // The image and its memory go with the texture.
    m_texture = nullptr;
    m_image = VK_NULL_HANDLE;
}

VulkanTransmissionCopyPass::VulkanTransmissionCopyPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanTransmissionImage& image)
    : m_nvrhiDevice(nvrhiDevice),
      m_image(image)
{
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(64),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(TransmissionCopyConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the transmission copy binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "transmission_copy.comp.spv", {frameSetLayout, m_setLayout});
    CreateBindingSets(targets);
}

VulkanTransmissionCopyPass::~VulkanTransmissionCopyPass() = default;

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

    // The copy rests in SHADER_READ_ONLY_OPTIMAL, where the forward pipelines sample it; the states
    // below order this frame's writes after last frame's reads, and the scope's last one its reads
    // after them.
    nvrhi::ITexture* copy = m_image.GetTexture();
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{copy, nvrhi::ResourceStates::ShaderResource}});
    commandList->setTextureState(copy, nvrhi::TextureSubresourceSet(0, 1, 0, 1), nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ComputeState state;
    state.pipeline = m_pipeline;
    state.bindings = {frame.frameBindingSet, m_bindingSets.at(slot)};
    commandList->setComputeState(state);
    TransmissionCopyConstants constants{};
    constants.extent = glm::uvec2(VulkanTransmissionImage::kSize);
    commandList->setPushConstants(&constants, sizeof(constants));
    constexpr uint32_t kGroups = (VulkanTransmissionImage::kSize + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize;
    commandList->dispatch(kGroups, kGroups);

    // Each level is blitted from the one above: NVRHI has no blit, so it moves the two levels into
    // the copy states and the blit is native.
    const VkImage image = m_image.GetImage();
    int32_t size = static_cast<int32_t>(VulkanTransmissionImage::kSize);
    for (uint32_t level = 1; level < VulkanTransmissionImage::kMipLevels; ++level)
    {
        commandList->setTextureState(copy, nvrhi::TextureSubresourceSet(level - 1, 1, 0, 1), nvrhi::ResourceStates::CopySource);
        commandList->setTextureState(copy, nvrhi::TextureSubresourceSet(level, 1, 0, 1), nvrhi::ResourceStates::CopyDest);
        commandList->commitBarriers();
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
        size = next;
    }
}

void VulkanTransmissionCopyPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateBindingSets(targets);
}

void VulkanTransmissionCopyPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneHdr, slot)),
            nvrhi::BindingSetItem::Sampler(64, m_sampler),
            nvrhi::BindingSetItem::Texture_UAV(1, m_image.GetTexture(), nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(TransmissionCopyConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a transmission copy binding set"));
    }
}
}
