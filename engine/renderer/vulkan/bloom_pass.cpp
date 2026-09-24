#include "bloom_pass.h"

#include <engine/renderer/bloom_chain.h>

#include <algorithm>
#include <array>

namespace me
{

namespace
{
constexpr VkFormat kChainFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match BloomConstants in shaders/vulkan/bloom.comp.
struct BloomPushConstants
{
    glm::uvec2 destinationExtent{0u};
    glm::vec2 sourceTexelSize{0.0f};
    uint32_t mode = 0;
    float intensity = 0.0f;
    float unused = 0.0f;
    float levelCount = 1.0f;
};
static_assert(sizeof(BloomPushConstants) == 32, "BloomPushConstants must match bloom.comp");

// Must match the MODE_* constants in bloom.comp.
constexpr uint32_t kModeFirstDownsample = 0u;
constexpr uint32_t kModeDownsample = 1u;
constexpr uint32_t kModeUpsample = 2u;
constexpr uint32_t kModeComposite = 3u;

// Everything one bloom dispatch wrote is visible to the next one's reads and ordered before its
// writes. The chain and SceneTaa are the only things written, both by this pass's compute.
void ComputeToComputeBarrier(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &barrier,
        0,
        nullptr,
        0,
        nullptr);
}

VkExtent2D ToExtent(glm::uvec2 size)
{
    return VkExtent2D{size.x, size.y};
}
}

VulkanBloomPass::VulkanBloomPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
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
            "bloom.comp.spv",
            sizeof(BloomPushConstants),
            m_pipelineLayout,
            m_pipeline);
        // Two sets per frame slot plus two per level at most; the pool is sized once for the
        // deepest chain so a resize never needs a new one.
        const uint32_t setCount = targets.GetTransientCopyCount() * 2 + kMaxBloomLevels * 2;
        m_descriptorPool = CreateImageDescriptorPool(m_device, setCount, 1, 1);
        CreateChain(targets.GetExtent());
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanBloomPass::~VulkanBloomPass()
{
    DestroyHandles();
}

ScenePassId VulkanBloomPass::Id() const
{
    return ScenePassId::Bloom;
}

RenderPassIo VulkanBloomPass::Io() const
{
    // SceneTaa is sampled and stored in place, both in GENERAL, which a write declaration puts it in
    // and orders after TAA's store.
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneTaa};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanBloomPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.bloom.enabled)
    {
        return;
    }

    // Last frame's chain is discarded: UNDEFINED to GENERAL, after every earlier use on the queue.
    VkImageMemoryBarrier discard{};
    discard.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    discard.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    discard.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    discard.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    discard.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    discard.image = m_chainImage;
    discard.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(m_levelExtents.size()), 0, 1};
    discard.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    discard.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &discard);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    const glm::uvec2 sceneExtent(frame.extent.width, frame.extent.height);
    BloomPushConstants constants{};
    constants.intensity = std::clamp(frame.bloom.intensity, 0.0f, 1.0f);
    constants.levelCount = static_cast<float>(m_levelExtents.size());
    const auto dispatch = [&](VkDescriptorSet set, uint32_t mode, glm::uvec2 source, glm::uvec2 destination)
    {
        constants.mode = mode;
        constants.sourceTexelSize = 1.0f / glm::vec2(source);
        constants.destinationExtent = destination;
        DispatchCompute(
            commandBuffer,
            m_pipeline,
            m_pipelineLayout,
            frame.frameDescriptorSet,
            set,
            &constants,
            sizeof(constants),
            ToExtent(destination));
        ComputeToComputeBarrier(commandBuffer);
    };

    dispatch(m_firstDownsampleSets.at(slot), kModeFirstDownsample, sceneExtent, m_levelExtents[0]);
    for (size_t level = 1; level < m_levelExtents.size(); ++level)
    {
        dispatch(m_downsampleSets[level - 1], kModeDownsample, m_levelExtents[level - 1], m_levelExtents[level]);
    }
    for (size_t level = m_levelExtents.size() - 1; level-- > 0;)
    {
        dispatch(m_upsampleSets[level], kModeUpsample, m_levelExtents[level + 1], m_levelExtents[level]);
    }
    dispatch(m_compositeSets.at(slot), kModeComposite, m_levelExtents[0], sceneExtent);
}

void VulkanBloomPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateChain(targets.GetExtent());
    CreateDescriptorSets(targets);
}

void VulkanBloomPass::CreateChain(VkExtent2D extent)
{
    DestroyChain();
    m_levelExtents = BuildBloomMipChain(glm::uvec2(extent.width, extent.height));

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {m_levelExtents[0].x, m_levelExtents[0].y, 1};
    imageInfo.mipLevels = static_cast<uint32_t>(m_levelExtents.size());
    imageInfo.arrayLayers = 1;
    imageInfo.format = kChainFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &m_chainImage), "Failed to create the bloom chain");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, m_chainImage, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(m_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_chainMemory), "Failed to allocate the bloom chain");
    CheckVulkan(vkBindImageMemory(m_device, m_chainImage, m_chainMemory, 0), "Failed to bind the bloom chain");

    // One view per level: a storage view may name only one level, and sampling one level through
    // its own view keeps the downsample from reading a level it is writing.
    for (uint32_t level = 0; level < static_cast<uint32_t>(m_levelExtents.size()); ++level)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_chainImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kChainFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &view), "Failed to create a bloom level view");
        m_levelViews.push_back(view);
    }
}

void VulkanBloomPass::DestroyChain()
{
    for (VkImageView view : m_levelViews)
    {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_levelViews.clear();
    if (m_chainImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_chainImage, nullptr);
        m_chainImage = VK_NULL_HANDLE;
    }
    if (m_chainMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_chainMemory, nullptr);
        m_chainMemory = VK_NULL_HANDLE;
    }
    m_levelExtents.clear();
}

void VulkanBloomPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    const uint32_t levelCount = static_cast<uint32_t>(m_levelViews.size());
    const uint32_t setCount = copyCount * 2 + (levelCount - 1) * 2;
    const std::vector<VkDescriptorSet> sets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, setCount);

    const auto write = [&](VkDescriptorSet set, VkImageView source, VkImageView destination)
    {
        const VkDescriptorImageInfo sourceInfo{m_sampler, source, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo destinationInfo{VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 2> writes = {
            ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sourceInfo),
            ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destinationInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    size_t next = 0;
    m_firstDownsampleSets.assign(sets.begin(), sets.begin() + copyCount);
    m_compositeSets.assign(sets.begin() + copyCount, sets.begin() + copyCount * 2);
    next = copyCount * 2;
    m_downsampleSets.assign(sets.begin() + next, sets.begin() + next + (levelCount - 1));
    next += levelCount - 1;
    m_upsampleSets.assign(sets.begin() + next, sets.begin() + next + (levelCount - 1));

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        // SceneTaa is sampled and stored through its storage view: the pass holds it in GENERAL.
        const VkImageView scene = targets.GetView(RenderTargetId::SceneTaa, slot);
        write(m_firstDownsampleSets[slot], scene, m_levelViews[0]);
        write(m_compositeSets[slot], m_levelViews[0], scene);
    }
    for (uint32_t level = 1; level < levelCount; ++level)
    {
        write(m_downsampleSets[level - 1], m_levelViews[level - 1], m_levelViews[level]);
        write(m_upsampleSets[level - 1], m_levelViews[level], m_levelViews[level - 1]);
    }
}

void VulkanBloomPass::DestroyHandles()
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
    m_firstDownsampleSets.clear();
    m_compositeSets.clear();
    m_downsampleSets.clear();
    m_upsampleSets.clear();
    DestroyChain();
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
