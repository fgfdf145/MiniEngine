#include "rt_shadow_pass.h"

#include "ray_scene.h"

#include <array>

namespace me
{

namespace
{
// Must match RtShadowConstants in shaders/vulkan/rt_shadow_common.glsl.
struct RtShadowPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
    float unused0 = 0.0f;
    float unused1 = 0.0f;
};
static_assert(sizeof(RtShadowPushConstants) == 32, "RtShadowPushConstants must match rt_shadow_common.glsl");

// Must match the RT_SHADOW_FLAG_* constants in rt_shadow_common.glsl.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagDenoise = 2u;
constexpr uint32_t kFlagHistoryValid = 4u;

constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kBindingCount = 8;

void ComputeBarrier(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Dispatch(VkCommandBuffer commandBuffer, VkExtent2D extent)
{
    vkCmdDispatch(
        commandBuffer,
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        1);
}
}

VulkanRtShadowPass::VulkanRtShadowPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        m_nearestSampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, kBindingCount> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
        CreateComputePipeline(
            m_device, pipelineCache, setLayouts, "rt_shadow_filter.comp.spv", sizeof(RtShadowPushConstants), m_pipelineLayout, m_filterPipeline);
        if (rayScene.HasHardwareRayTracing())
        {
            m_temporalPipeline = CreateComputeShaderPipeline(m_device, pipelineCache, m_pipelineLayout, "rt_shadow_temporal.comp.spv");
            const std::array<VkDescriptorSetLayout, 4> traceLayouts = {
                frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
            CreateComputePipeline(
                m_device, pipelineCache, traceLayouts, "rt_shadow_trace.comp.spv", sizeof(RtShadowPushConstants), m_tracePipelineLayout, m_tracePipeline);
        }
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 4, 4);
        m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanRtShadowPass::~VulkanRtShadowPass()
{
    DestroyHandles();
}

ScenePassId VulkanRtShadowPass::Id() const
{
    return ScenePassId::RtShadow;
}

RenderPassIo VulkanRtShadowPass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferVelocity};
    static constexpr std::array<RenderTargetId, 2> kWrites = {RenderTargetId::ShadowRaw, RenderTargetId::SceneShadow};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanRtShadowPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Even while nothing traces: the bound descriptors name both history images in GENERAL.
    const bool traced = frame.rayTracing.sunShadows && m_tracePipeline != VK_NULL_HANDLE;
    m_history.RecordBarrier(commandBuffer, traced && frame.rtShadowHistory.valid);

    RtShadowPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.frameIndex = frame.frameIndex;
    constants.flags = (traced ? kFlagEnabled : 0u) | (frame.rayTracing.denoise ? kFlagDenoise : 0u) |
                      (frame.rtShadowHistory.valid ? kFlagHistoryValid : 0u);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneShadow, frame.imageIndex, frame.frameSlot);
    const VkDescriptorSet passSet = m_descriptorSets.at(slot * 2 + frame.rtShadowHistory.readIndex);
    if (traced)
    {
        const std::array<VkDescriptorSet, 4> sets = {frame.frameDescriptorSet, frame.raySet, passSet, frame.rayTextureSet};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracePipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_tracePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        Dispatch(commandBuffer, frame.extent);
        ComputeBarrier(commandBuffer);
    }

    const std::array<VkDescriptorSet, 2> sets = {frame.frameDescriptorSet, passSet};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    if (traced && frame.rayTracing.denoise)
    {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_temporalPipeline);
        Dispatch(commandBuffer, frame.extent);
        ComputeBarrier(commandBuffer);
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_filterPipeline);
    Dispatch(commandBuffer, frame.extent);
}

void VulkanRtShadowPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the history bookkeeping at the same call sites.
    m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateDescriptorSets(targets);
}

void VulkanRtShadowPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo depthInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{m_nearestSampler, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::ShadowRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyReadInfo{m_linearSampler, m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo shadowInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneShadow, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, kBindingCount> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shadowInfo),
                ImageWrite(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanRtShadowPass::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_tracePipeline, &m_temporalPipeline, &m_filterPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    for (VkPipelineLayout* layout : {&m_tracePipelineLayout, &m_pipelineLayout})
    {
        if (*layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    m_history.Destroy();
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
