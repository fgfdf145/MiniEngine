#include "ao_pass.h"

#include "compute_pass_util.h"
#include "ray_scene.h"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
constexpr float kMaxPixelRadius = 256.0f;
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// vbao_resolve.comp's historyTexture, the one input it samples rather than loads.
constexpr uint32_t kHistoryReadBinding = 3;

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
constexpr uint32_t kFlagRayTracedAo = 16u;
constexpr uint32_t kFlagProbeOcclusion = 32u;

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
        (frame.aoHistory.valid ? kFlagHistoryValid : 0u) |
        (frame.rayTracing.ambientOcclusion ? kFlagRayTracedAo : 0u) |
        (frame.rayTracing.probeOcclusion ? kFlagProbeOcclusion : 0u);
    return constants;
}

}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanAoTracePass::VulkanAoTracePass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_device(device)
{
    try
    {
        static constexpr std::array<VkDescriptorType, 3> kTypes = {
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "vbao_trace.comp.spv", sizeof(AoPushConstants), m_pipelineLayout, m_pipeline);
        if (rayScene.HasHardwareRayTracing())
        {
            const std::array<VkDescriptorSetLayout, 4> setLayouts = {
                frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
            CreateComputePipeline(
                m_device, pipelineCache, setLayouts, "rt_occlusion.comp.spv", sizeof(AoPushConstants), m_tracedPipelineLayout, m_tracedPipeline);
        }
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 2, 1);
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
    const bool traced = m_tracedPipeline != VK_NULL_HANDLE && (frame.rayTracing.ambientOcclusion || frame.rayTracing.probeOcclusion);
    const bool bitmask = frame.ao.enabled && !frame.rayTracing.ambientOcclusion;
    if (!traced && !bitmask)
    {
        return;
    }
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::AoRaw, frame.imageIndex, frame.frameSlot);
    const VkExtent2D extent = targets.GetTargetExtent(RenderTargetId::AoRaw);
    AoPushConstants constants = BuildPushConstants(frame);
    if (bitmask)
    {
        DispatchCompute(
            commandBuffer,
            m_pipeline,
            m_pipelineLayout,
            frame.frameDescriptorSet,
            m_descriptorSets.at(slot),
            &constants,
            sizeof(constants),
            extent);
    }
    if (!traced)
    {
        return;
    }
    if (bitmask)
    {
        // The traced pass reads the bitmask's AO back and adds the probe occlusion beside it.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    // The traced rays per pixel ride in the slice count.
    constants.sliceCount = static_cast<uint32_t>(frame.rayTracing.occlusionRays);
    const std::array<VkDescriptorSet, 4> sets = {frame.frameDescriptorSet, frame.raySet, m_descriptorSets.at(slot), frame.rayTextureSet};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracedPipeline);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracedPipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(commandBuffer, m_tracedPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(
        commandBuffer,
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        1);
}

void VulkanAoTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanAoTracePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
        const VkDescriptorImageInfo normalInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
        const VkDescriptorImageInfo aoInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::AoRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 3> writes = {
            ImageWrite(m_descriptorSets[slot], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo),
            ImageWrite(m_descriptorSets[slot], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &normalInfo),
            ImageWrite(m_descriptorSets[slot], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aoInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanAoTracePass::DestroyHandles()
{
    if (m_tracedPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_tracedPipeline, nullptr);
        m_tracedPipeline = VK_NULL_HANDLE;
    }
    if (m_tracedPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_tracedPipelineLayout, nullptr);
        m_tracedPipelineLayout = VK_NULL_HANDLE;
    }
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
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanAoResolvePass::VulkanAoResolvePass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    try
    {
        m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 6> kTypes = {
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        // The history is sampled (bilinear reprojection); the rest are loaded.
        static constexpr std::array<uint32_t, 1> kSampled = {kHistoryReadBinding};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes, kSampled);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "vbao_resolve.comp.spv", sizeof(AoPushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 4, 2, 1);
        m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
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
    // Runs even with AO off: the bound descriptors name both images in GENERAL.
    m_history.RecordBarrier(commandBuffer, frame.aoHistory.valid);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneAo, frame.imageIndex, frame.frameSlot);
    const AoPushConstants constants = BuildPushConstants(frame);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.aoHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanAoResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets TemporalHistory at the same call sites, so the next frame discards the new
    // images' undefined contents.
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateDescriptorSets(targets);
}

void VulkanAoResolvePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo aoRawInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::AoRaw, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{VK_NULL_HANDLE, m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historySamplerInfo{NativeSampler(m_linearSampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo aoInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneAo, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 7> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &aoRawInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &velocityInfo),
                ImageWrite(set, kHistoryReadBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &historyReadInfo),
                ImageWrite(set, kSplitSamplerBindingOffset + kHistoryReadBinding, VK_DESCRIPTOR_TYPE_SAMPLER, &historySamplerInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &aoInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
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
    m_history.Destroy();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    m_linearSampler = nullptr;
}
}
