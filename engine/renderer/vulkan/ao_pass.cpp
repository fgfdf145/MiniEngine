#include "ao_pass.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
// Must match local_size_x / local_size_y in vbao_trace.comp and vbao_resolve.comp.
constexpr uint32_t kWorkgroupSize = 8;
constexpr float kMaxPixelRadius = 256.0f;
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match AoConstants in shaders/vulkan/vbao_common.glsl.
struct AoPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float radius = 0.0f;
    float thickness = 0.0f;
    float maxPixelRadius = 0.0f;
    float unused = 0.0f;
    uint32_t sliceCount = 0;
    uint32_t stepCount = 0;
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(AoPushConstants) == 48, "AoPushConstants must match vbao_common.glsl");

// Must match the AO_FLAG_* constants in vbao_common.glsl.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagSpatial = 2u;
constexpr uint32_t kFlagTemporal = 4u;
constexpr uint32_t kFlagHistoryValid = 8u;

// Clamps every setting to the range the editor offers, so a value from anywhere else cannot reach
// the shader.
AoPushConstants BuildPushConstants(const ScenePassFrameContext& frame)
{
    AoPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.radius = std::clamp(frame.ao.radius, 0.1f, 5.0f);
    constants.thickness = std::clamp(frame.ao.thickness, 0.01f, 2.0f);
    constants.maxPixelRadius = kMaxPixelRadius;
    constants.sliceCount = static_cast<uint32_t>(std::clamp(frame.ao.sliceCount, 1, 4));
    constants.stepCount = static_cast<uint32_t>(std::clamp(frame.ao.stepCount, 2, 16));
    constants.frameIndex = frame.frameIndex;
    constants.flags =
        (frame.ao.enabled ? kFlagEnabled : 0u) |
        (frame.ao.spatialFilter ? kFlagSpatial : 0u) |
        (frame.ao.temporalFilter ? kFlagTemporal : 0u) |
        (frame.aoHistory.valid ? kFlagHistoryValid : 0u);
    return constants;
}

VkSampler CreateSampler(VkDevice device, VkFilter filter)
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSampler sampler = VK_NULL_HANDLE;
    CheckVulkan(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "Failed to create AO sampler");
    return sampler;
}

VkDescriptorSetLayout CreateSetLayout(VkDevice device, std::span<const VkDescriptorType> types)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (uint32_t binding = 0; binding < static_cast<uint32_t>(types.size()); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = types[binding];
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    CheckVulkan(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "Failed to create AO descriptor set layout");
    return layout;
}

void CreateComputePipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout passSetLayout,
    const char* shaderName,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline)
{
    const VulkanShaderModule computeShader(device, EnginePaths::ShaderRoot() / shaderName);

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(AoPushConstants);

    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, passSetLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "Failed to create AO pipeline layout");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShader.GetHandle();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    CheckVulkan(
        vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline),
        "Failed to create AO pipeline");
}

void Dispatch(
    VkCommandBuffer commandBuffer,
    VkPipeline pipeline,
    VkPipelineLayout pipelineLayout,
    VkDescriptorSet frameSet,
    VkDescriptorSet passSet,
    const AoPushConstants& constants,
    VkExtent2D extent)
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const std::array<VkDescriptorSet, 2> sets = {frameSet, passSet};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout,
        0,
        static_cast<uint32_t>(sets.size()),
        sets.data(),
        0,
        nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(
        commandBuffer,
        (extent.width + kWorkgroupSize - 1) / kWorkgroupSize,
        (extent.height + kWorkgroupSize - 1) / kWorkgroupSize,
        1);
}

VkDescriptorPool CreatePool(VkDevice device, uint32_t setCount, uint32_t samplersPerSet, uint32_t storagePerSet)
{
    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * samplersPerSet},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount * storagePerSet}};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckVulkan(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "Failed to create AO descriptor pool");
    return pool;
}

std::vector<VkDescriptorSet> AllocateSets(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t count)
{
    CheckVulkan(vkResetDescriptorPool(device, pool, 0), "Failed to reset AO descriptor pool");
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool;
    allocateInfo.descriptorSetCount = count;
    allocateInfo.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(count, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(device, &allocateInfo, sets.data()), "Failed to allocate AO descriptor sets");
    return sets;
}

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = info;
    return write;
}
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanAoTracePass::VulkanAoTracePass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_device(device)
{
    try
    {
        m_sampler = CreateSampler(m_device, VK_FILTER_NEAREST);
        static constexpr std::array<VkDescriptorType, 3> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "vbao_trace.comp.spv", m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreatePool(m_device, targets.GetTransientCopyCount(), 2, 1);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanAoTracePass::~VulkanAoTracePass()
{
    DestroyHandles();
}

ScenePassId VulkanAoTracePass::Id() const
{
    return ScenePassId::AoTrace;
}

RenderPassIo VulkanAoTracePass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {RenderTargetId::SceneDepth, RenderTargetId::GBufferNormal};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::AoRaw};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanAoTracePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.ao.enabled)
    {
        return;
    }
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::AoRaw, frame.imageIndex, frame.frameSlot);
    Dispatch(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot),
        BuildPushConstants(frame),
        frame.extent);
}

void VulkanAoTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanAoTracePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateSets(m_device, m_descriptorPool, m_setLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorImageInfo depthInfo{m_sampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
        const VkDescriptorImageInfo normalInfo{m_sampler, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
        const VkDescriptorImageInfo aoInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::AoRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 3> writes = {
            ImageWrite(m_descriptorSets[slot], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
            ImageWrite(m_descriptorSets[slot], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
            ImageWrite(m_descriptorSets[slot], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aoInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanAoTracePass::DestroyHandles()
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
    m_descriptorSets.clear();
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

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanAoResolvePass::VulkanAoResolvePass(
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
        m_nearestSampler = CreateSampler(m_device, VK_FILTER_NEAREST);
        m_linearSampler = CreateSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 6> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "vbao_resolve.comp.spv", m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreatePool(m_device, targets.GetTransientCopyCount() * 2, 4, 2);
        CreateHistoryImages(targets.GetExtent());
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanAoResolvePass::~VulkanAoResolvePass()
{
    DestroyHandles();
}

ScenePassId VulkanAoResolvePass::Id() const
{
    return ScenePassId::AoResolve;
}

RenderPassIo VulkanAoResolvePass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kReads = {
        RenderTargetId::AoRaw,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneAo};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanAoResolvePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Both history images, compute to compute. Invalid history is discarded with an UNDEFINED to
    // GENERAL transition, which is also the one a freshly created image needs; valid history keeps
    // its contents behind a GENERAL to GENERAL barrier that makes last frame's store visible and
    // orders this frame's store after last frame's sample. A barrier's first scope covers every
    // command submitted earlier on the queue, so this reaches across command buffers. It runs even
    // with AO off: the bound descriptors name both images in GENERAL, and validation checks that.
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (size_t index = 0; index < barriers.size(); ++index)
    {
        VkImageMemoryBarrier& barrier = barriers[index];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = frame.aoHistory.valid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_history[index].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneAo, frame.imageIndex, frame.frameSlot);
    Dispatch(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.aoHistory.readIndex),
        BuildPushConstants(frame),
        frame.extent);
}

void VulkanAoResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets TemporalHistory at the same call sites, so the next frame discards the new
    // images' undefined contents.
    DestroyHistoryImages();
    CreateHistoryImages(targets.GetExtent());
    CreateDescriptorSets(targets);
}

void VulkanAoResolvePass::CreateHistoryImages(VkExtent2D extent)
{
    for (HistoryImage& history : m_history)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kHistoryFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &history.image), "Failed to create AO history image");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, history.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &history.memory), "Failed to allocate AO history memory");
        CheckVulkan(vkBindImageMemory(m_device, history.image, history.memory, 0), "Failed to bind AO history memory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = history.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kHistoryFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &history.view), "Failed to create AO history view");
    }
}

void VulkanAoResolvePass::DestroyHistoryImages()
{
    for (HistoryImage& history : m_history)
    {
        if (history.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, history.view, nullptr);
        }
        if (history.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, history.image, nullptr);
        }
        if (history.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, history.memory, nullptr);
        }
        history = HistoryImage{};
    }
}

void VulkanAoResolvePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo aoRawInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::AoRaw, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{m_linearSampler, m_history[readIndex].view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history[1u - readIndex].view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo aoInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneAo, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 6> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aoRawInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aoInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

uint32_t VulkanAoResolvePass::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("Failed to find a memory type for the AO history");
}

void VulkanAoResolvePass::DestroyHandles()
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
    m_descriptorSets.clear();
    DestroyHistoryImages();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    for (VkSampler* sampler : {&m_nearestSampler, &m_linearSampler})
    {
        if (*sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
}
}
