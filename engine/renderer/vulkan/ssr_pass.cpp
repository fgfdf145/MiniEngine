#include "ssr_pass.h"

#include "taa_pass.h"

#include <algorithm>
#include <array>

namespace me
{

namespace
{
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match SsrConstants in shaders/vulkan/ssr_trace.comp.
struct SsrPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float maxDistance = 30.0f;
    float maxRoughness = 0.8f;
    float historyScale = 1.0f;
    uint32_t frameIndex = 0;
};
static_assert(sizeof(SsrPushConstants) == 32, "SsrPushConstants must match ssr_trace.comp");

// Must match SsrResolveConstants in shaders/vulkan/ssr_resolve.comp.
struct SsrResolvePushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float historyScale = 1.0f;
    uint32_t flags = 0;
    glm::vec2 unused{0.0f};
};
static_assert(sizeof(SsrResolvePushConstants) == 32, "SsrResolvePushConstants must match ssr_resolve.comp");

// Must match the SSR_FLAG_* constants in ssr_resolve.comp.
constexpr uint32_t kFlagTraced = 1u;
constexpr uint32_t kFlagHistoryValid = 2u;

glm::vec2 Extent(const ScenePassFrameContext& frame)
{
    return glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
}

void DestroyCommon(
    VkDevice device,
    VkPipeline& pipeline,
    VkPipelineLayout& pipelineLayout,
    VkDescriptorPool& descriptorPool,
    VkDescriptorSetLayout& setLayout,
    std::initializer_list<VkSampler*> samplers)
{
    if (pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
    if (descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }
    if (setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        setLayout = VK_NULL_HANDLE;
    }
    for (VkSampler* sampler : samplers)
    {
        if (*sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
}
}

bool SsrTraces(const ScenePassFrameContext& frame)
{
    return frame.ssr.enabled && frame.taaHistory.valid;
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanSsrTracePass::VulkanSsrTracePass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanTaaPass& taa)
    : m_device(device),
      m_taa(taa)
{
    try
    {
        m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 5> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "ssr_trace.comp.spv", sizeof(SsrPushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 4, 1);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanSsrTracePass::~VulkanSsrTracePass()
{
    DestroyHandles();
}

ScenePassId VulkanSsrTracePass::Id() const
{
    return ScenePassId::SsrTrace;
}

RenderPassIo VulkanSsrTracePass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SsrRaw};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanSsrTracePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Without valid history the TAA images may still be UNDEFINED; they are not touched.
    if (!SsrTraces(frame))
    {
        return;
    }

    // Last frame's TAA resolve stored the history this reads, in an earlier submission; TAA's own
    // barrier for it comes later in this frame, so this read needs its own.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
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

    SsrPushConstants constants{};
    constants.extent = Extent(frame);
    constants.invExtent = 1.0f / constants.extent;
    constants.maxDistance = std::clamp(frame.ssr.maxDistance, 1.0f, 200.0f);
    constants.maxRoughness = std::clamp(frame.ssr.maxRoughness, 0.05f, 1.0f);
    constants.historyScale = frame.taaHistoryScale;
    constants.frameIndex = frame.frameIndex;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SsrRaw, frame.imageIndex, frame.frameSlot);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.taaHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanSsrTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanSsrTracePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + historyIndex];
            const VkDescriptorImageInfo depthInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo surfaceInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferSurface, slot), kReadLayout};
            const VkDescriptorImageInfo historyInfo{m_linearSampler, m_taa.GetHistoryView(historyIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SsrRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 5> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &surfaceInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanSsrTracePass::DestroyHandles()
{
    m_descriptorSets.clear();
    DestroyCommon(m_device, m_pipeline, m_pipelineLayout, m_descriptorPool, m_setLayout, {&m_nearestSampler, &m_linearSampler});
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanSsrResolvePass::VulkanSsrResolvePass(
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
        m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 8> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "ssr_resolve.comp.spv", sizeof(SsrResolvePushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 6, 2);
        m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanSsrResolvePass::~VulkanSsrResolvePass()
{
    DestroyHandles();
}

ScenePassId VulkanSsrResolvePass::Id() const
{
    return ScenePassId::SsrResolve;
}

RenderPassIo VulkanSsrResolvePass::Io() const
{
    static constexpr std::array<RenderTargetId, 5> kReads = {
        RenderTargetId::SsrRaw,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferVelocity};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneReflections};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanSsrResolvePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Runs even with SSR off: the bound descriptors name both images in GENERAL.
    m_history.RecordBarrier(commandBuffer, frame.ssrHistory.valid);

    SsrResolvePushConstants constants{};
    constants.extent = Extent(frame);
    constants.invExtent = 1.0f / constants.extent;
    // The resolve's history was written last frame, at last frame's pre-exposure, like TAA's.
    constants.historyScale = frame.taaHistoryScale;
    constants.flags = (SsrTraces(frame) ? kFlagTraced : 0u) | (frame.ssrHistory.valid ? kFlagHistoryValid : 0u);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneReflections, frame.imageIndex, frame.frameSlot);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.ssrHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanSsrResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the SSR TemporalHistory at the same call sites, so the next frame discards
    // the new images' undefined contents.
    m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateDescriptorSets(targets);
}

void VulkanSsrResolvePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo rawInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SsrRaw, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo surfaceInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferSurface, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{m_linearSampler, m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo reflectionsInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneReflections, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 8> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rawInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &surfaceInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &reflectionsInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanSsrResolvePass::DestroyHandles()
{
    m_descriptorSets.clear();
    m_history.Destroy();
    DestroyCommon(m_device, m_pipeline, m_pipelineLayout, m_descriptorPool, m_setLayout, {&m_nearestSampler, &m_linearSampler});
}
}
