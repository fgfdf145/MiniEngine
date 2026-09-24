#include "exposure_histogram_pass.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/renderer/exposure.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace me
{

namespace
{
// Must match local_size_x / local_size_y in exposure_histogram.comp.
constexpr uint32_t kWorkgroupSize = 16;
static_assert(
    kWorkgroupSize * kWorkgroupSize == kExposureHistogramBinCount,
    "the shader clears and flushes one bin per invocation");

constexpr VkDeviceSize kHistogramBytes = sizeof(uint32_t) * kExposureHistogramBinCount;

// Must match HistogramConstants in shaders/vulkan/exposure_histogram.comp.
struct HistogramPushConstants
{
    uint32_t width = 0;
    uint32_t height = 0;
    // 1 when the pixels no geometry covered hold a physical sky and are metered like the rest.
    uint32_t meterBackground = 0;
    uint32_t unused = 0;
};
}

VulkanExposureHistogramPass::VulkanExposureHistogramPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    // A throw out of a constructor skips the destructor, so everything created before the failure
    // would leak with it. DestroyHandles skips null handles, so unwinding whatever got created is
    // the same call the destructor makes.
    try
    {
        CreateDescriptorSetLayout();
        CreateSampler();
        CreatePipeline(pipelineCache);
        CreateHistogramBuffers(targets.GetTransientCopyCount());
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanExposureHistogramPass::~VulkanExposureHistogramPass()
{
    DestroyHandles();
}

ScenePassId VulkanExposureHistogramPass::Id() const
{
    return ScenePassId::ExposureHistogram;
}

RenderPassIo VulkanExposureHistogramPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {
        RenderTargetId::SceneTaa,
        RenderTargetId::SceneDepth};

    RenderPassIo io{};
    io.reads = kReads;
    return io;
}

void VulkanExposureHistogramPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // The resolved HDR and depth copies and the histogram buffer are all per frame slot, so one index
    // picks all three.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    const VkBuffer histogram = m_histograms.at(slot).buffer;

    // The CPU read this buffer before the frame was submitted, which vkQueueSubmit orders ahead of
    // the clear, so only the clear-to-atomics and atomics-to-host hazards need barriers.
    vkCmdFillBuffer(commandBuffer, histogram, 0, kHistogramBytes, 0);

    VkBufferMemoryBarrier clearToCompute{};
    clearToCompute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    clearToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    clearToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearToCompute.buffer = histogram;
    clearToCompute.offset = 0;
    clearToCompute.size = kHistogramBytes;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &clearToCompute,
        0,
        nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        m_pipelineLayout,
        0,
        1,
        &m_descriptorSets.at(slot),
        0,
        nullptr);

    const HistogramPushConstants constants{frame.extent.width, frame.extent.height, frame.physicalSky ? 1u : 0u, 0u};
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(constants),
        &constants);
    vkCmdDispatch(
        commandBuffer,
        (frame.extent.width + kWorkgroupSize - 1) / kWorkgroupSize,
        (frame.extent.height + kWorkgroupSize - 1) / kWorkgroupSize,
        1);

    VkBufferMemoryBarrier computeToHost{};
    computeToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    computeToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    computeToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    computeToHost.buffer = histogram;
    computeToHost.offset = 0;
    computeToHost.size = kHistogramBytes;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &computeToHost,
        0,
        nullptr);
}

void VulkanExposureHistogramPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The sets point at the old HDR and depth views. The histogram buffers do not depend on the
    // targets and keep their last results.
    CreateDescriptorSets(targets);
}

std::span<const uint32_t> VulkanExposureHistogramPass::GetHistogram(uint32_t frameSlot) const
{
    return std::span<const uint32_t>(m_histograms.at(frameSlot).mapped, kExposureHistogramBinCount);
}

void VulkanExposureHistogramPass::CreateDescriptorSetLayout()
{
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t binding = 0; binding < 2; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create exposure histogram descriptor set layout");
}

void VulkanExposureHistogramPass::CreateSampler()
{
    // The shader only uses texelFetch, which ignores the sampler's filtering, but a combined image
    // sampler still needs one. Nearest keeps it valid for depth formats without linear filtering.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create exposure histogram sampler");
}

void VulkanExposureHistogramPass::CreatePipeline(VkPipelineCache pipelineCache)
{
    const VulkanShaderModule computeShader(m_device, EnginePaths::ShaderRoot() / "exposure_histogram.comp.spv");

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(HistogramPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create exposure histogram pipeline layout");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShader.GetHandle();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_pipelineLayout;

    CheckVulkan(
        vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, &m_pipeline),
        "Failed to create exposure histogram pipeline");
}

void VulkanExposureHistogramPass::CreateHistogramBuffers(uint32_t count)
{
    m_histograms.reserve(count);
    for (uint32_t slot = 0; slot < count; ++slot)
    {
        // Appended before anything is created so a failure part way through still leaves every
        // handle created so far reachable by DestroyHandles.
        HistogramBuffer& histogram = m_histograms.emplace_back();

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kHistogramBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &histogram.buffer), "Failed to create exposure histogram buffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, histogram.buffer, &requirements);

        // Coherent, so the CPU sees the GPU's writes once the fence and the host barrier in Record
        // have run, with no invalidate.
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &histogram.memory), "Failed to allocate exposure histogram memory");
        CheckVulkan(vkBindBufferMemory(m_device, histogram.buffer, histogram.memory, 0), "Failed to bind exposure histogram memory");

        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(m_device, histogram.memory, 0, kHistogramBytes, 0, &mapped), "Failed to map exposure histogram memory");
        // Zeroed so a slot that has never been recorded reads as an empty histogram.
        std::memset(mapped, 0, static_cast<size_t>(kHistogramBytes));
        histogram.mapped = static_cast<const uint32_t*>(mapped);
    }
}

void VulkanExposureHistogramPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    // One set per frame slot. The count is fixed (kMaxFramesInFlight), so the pool is sized once
    // and reset when the target views change.
    const uint32_t copyCount = targets.GetTransientCopyCount();

    if (m_descriptorPool == VK_NULL_HANDLE)
    {
        const std::array<VkDescriptorPoolSize, 2> poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * copyCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, copyCount}};

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = copyCount;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create exposure histogram descriptor pool");
    }
    else
    {
        m_descriptorSets.clear();
        CheckVulkan(vkResetDescriptorPool(m_device, m_descriptorPool, 0), "Failed to reset exposure histogram descriptor pool");
    }

    const std::vector<VkDescriptorSetLayout> layouts(copyCount, m_setLayout);

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = copyCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.assign(copyCount, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate exposure histogram descriptor sets");

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        VkDescriptorImageInfo hdrInfo{};
        hdrInfo.sampler = m_sampler;
        hdrInfo.imageView = targets.GetSampledView(RenderTargetId::SceneTaa, slot);
        hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = m_sampler;
        depthInfo.imageView = targets.GetSampledView(RenderTargetId::SceneDepth, slot);
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo histogramInfo{};
        histogramInfo.buffer = m_histograms.at(slot).buffer;
        histogramInfo.offset = 0;
        histogramInfo.range = kHistogramBytes;

        std::array<VkWriteDescriptorSet, 3> writes{};
        for (VkWriteDescriptorSet& write : writes)
        {
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSets[slot];
            write.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &hdrInfo;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &depthInfo;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &histogramInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

uint32_t VulkanExposureHistogramPass::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
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

    throw std::runtime_error("Failed to find a host-visible memory type for the exposure histogram");
}

void VulkanExposureHistogramPass::DestroyHandles()
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
    // Destroying the pool frees every set allocated from it.
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    for (HistogramBuffer& histogram : m_histograms)
    {
        if (histogram.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, histogram.buffer, nullptr);
        }
        // Freeing mapped memory unmaps it implicitly.
        if (histogram.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, histogram.memory, nullptr);
        }
    }
    m_histograms.clear();
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
