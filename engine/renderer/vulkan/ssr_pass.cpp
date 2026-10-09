#include "ssr_pass.h"

#include "ray_scene.h"
#include "taa_pass.h"

#include <algorithm>
#include <array>

namespace me
{

namespace
{
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The inputs the shaders sample rather than load: ssr_trace.comp's depth (nearest) and TAA history
// (linear; rt_reflection_trace.comp samples it too), ssr_resolve.comp's history (linear).
constexpr uint32_t kTraceDepthBinding = 0;
constexpr uint32_t kTraceHistoryBinding = 3;
constexpr uint32_t kResolveHistoryBinding = 5;
// How far a ray traced reflection looks, in metres.
constexpr float kTracedReflectionDistance = 5000.0f;

// Must match SsrConstants in shaders/vulkan/ssr_trace.comp.
struct SsrPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float maxDistance = 30.0f;
    float maxRoughness = 0.8f;
    float historyScale = 1.0f;
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(SsrPushConstants) == 36, "SsrPushConstants must match ssr_trace.comp");

// Must match SSR_TRACE_FLAG_* in shaders/vulkan/ssr_half_res.glsl.
constexpr uint32_t kTraceFlagFullResolution = 1u;

// Must match SsrResolveConstants in shaders/vulkan/ssr_resolve.comp.
struct SsrResolvePushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float historyScale = 1.0f;
    uint32_t flags = 0;
    // The trace's, for which pixel of each 2x2 block it traced (ssr_half_res.glsl).
    uint32_t frameIndex = 0;
    float unused = 0.0f;
};
static_assert(sizeof(SsrResolvePushConstants) == 32, "SsrResolvePushConstants must match ssr_resolve.comp");

// Must match the SSR_FLAG_* constants in ssr_resolve.comp.
constexpr uint32_t kFlagTraced = 1u;
constexpr uint32_t kFlagHistoryValid = 2u;
constexpr uint32_t kFlagPassThrough = 4u;

glm::vec2 Extent(const ScenePassFrameContext& frame)
{
    return glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
}

void DestroyCommon(
    VkDevice device,
    VkPipeline& pipeline,
    VkPipelineLayout& pipelineLayout,
    VkDescriptorPool& descriptorPool,
    VkDescriptorSetLayout& setLayout)
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
}
}

bool SsrTraces(const ScenePassFrameContext& frame)
{
    return (frame.ssr.enabled || frame.rayTracing.reflections) && frame.taaHistory.valid;
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanSsrTracePass::VulkanSsrTracePass(
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    const VulkanTaaPass& taa,
    const VulkanRayScene& rayScene)
    : m_device(device),
      m_taa(taa)
{
    try
    {
        m_nearestSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
        m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
        static constexpr std::array<VkDescriptorType, 7> kTypes = {
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
        static constexpr std::array<uint32_t, 2> kSampled = {kTraceDepthBinding, kTraceHistoryBinding};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes, kSampled);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "ssr_trace.comp.spv", sizeof(SsrPushConstants), m_pipelineLayout, m_pipeline);
        if (rayScene.HasHardwareRayTracing())
        {
            const std::array<VkDescriptorSetLayout, 4> setLayouts = {
                frameSetLayout, rayScene.GetSetLayout(), m_setLayout, rayScene.GetTextureSetLayout()};
            CreateComputePipeline(
                m_device, pipelineCache, setLayouts, "rt_reflection_trace.comp.spv", sizeof(SsrPushConstants), m_tracedPipelineLayout, m_tracedPipeline);
        }
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 6, 1, 2);
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
    // The coat target and the velocity target's coat normal: a coated pixel traces its coat's lobe
    // (ssr_lobe.glsl).
    static constexpr std::array<RenderTargetId, 5> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferVelocity};
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
    // DLSS ray reconstruction denoises the reflections itself and wants a raw sample in every pixel
    // (ssr_half_res.glsl); otherwise half resolution, filtered by the resolve.
    constants.flags = frame.dlssRayReconstruction ? kTraceFlagFullResolution : 0u;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SsrRaw, frame.imageIndex, frame.frameSlot);
    const VkDescriptorSet passSet = m_descriptorSets.at(slot * 2 + frame.taaHistory.readIndex);
    const VkExtent2D extent = frame.dlssRayReconstruction ? frame.extent : VkExtent2D{(frame.extent.width + 1) / 2, (frame.extent.height + 1) / 2};
    if (frame.rayTracing.reflections && m_tracedPipeline != VK_NULL_HANDLE)
    {
        // The scene, not the screen: the rays reach as far as the sky.
        constants.maxDistance = kTracedReflectionDistance;
        const std::array<VkDescriptorSet, 4> sets = {frame.frameDescriptorSet, frame.raySet, passSet, frame.rayTextureSet};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracedPipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tracedPipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_tracedPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(
            commandBuffer,
            (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
            (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
            1);
        return;
    }
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        passSet,
        &constants,
        sizeof(constants),
        extent);
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
            const VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo surfaceInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferSurface, slot), kReadLayout};
            const VkDescriptorImageInfo historyInfo{VK_NULL_HANDLE, m_taa.GetHistoryView(historyIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SsrRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo coatInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferCoat, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo nearestInfo{NativeSampler(m_nearestSampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorImageInfo linearInfo{NativeSampler(m_linearSampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const std::array<VkWriteDescriptorSet, 9> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &normalInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &surfaceInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &historyInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &coatInfo),
                ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &velocityInfo),
                ImageWrite(set, kSplitSamplerBindingOffset + kTraceDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLER, &nearestInfo),
                ImageWrite(set, kSplitSamplerBindingOffset + kTraceHistoryBinding, VK_DESCRIPTOR_TYPE_SAMPLER, &linearInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanSsrTracePass::DestroyHandles()
{
    m_descriptorSets.clear();
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
    DestroyCommon(m_device, m_pipeline, m_pipelineLayout, m_descriptorPool, m_setLayout);
    m_nearestSampler = nullptr;
    m_linearSampler = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanSsrResolvePass::VulkanSsrResolvePass(
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
        static constexpr std::array<VkDescriptorType, 9> kTypes = {
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
        static constexpr std::array<uint32_t, 1> kSampled = {kResolveHistoryBinding};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes, kSampled);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "ssr_resolve.comp.spv", sizeof(SsrResolvePushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 7, 2, 1);
        m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
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
    static constexpr std::array<RenderTargetId, 6> kReads = {
        RenderTargetId::SsrRaw,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferCoat};
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
    constants.flags = (SsrTraces(frame) ? kFlagTraced : 0u) | (frame.ssrHistory.valid ? kFlagHistoryValid : 0u) |
                      (frame.dlssRayReconstruction ? kFlagPassThrough : 0u);
    constants.frameIndex = frame.frameIndex;

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
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
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
            const VkDescriptorImageInfo rawInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::SsrRaw, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo surfaceInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferSurface, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{VK_NULL_HANDLE, m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo reflectionsInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneReflections, slot), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo coatInfo{VK_NULL_HANDLE, targets.GetSampledView(RenderTargetId::GBufferCoat, slot), kReadLayout};
            const VkDescriptorImageInfo linearInfo{NativeSampler(m_linearSampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            const std::array<VkWriteDescriptorSet, 10> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &rawInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &normalInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &surfaceInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &velocityInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &historyReadInfo),
                ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &reflectionsInfo),
                ImageWrite(set, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &coatInfo),
                ImageWrite(set, kSplitSamplerBindingOffset + kResolveHistoryBinding, VK_DESCRIPTOR_TYPE_SAMPLER, &linearInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanSsrResolvePass::DestroyHandles()
{
    m_descriptorSets.clear();
    m_history.Destroy();
    DestroyCommon(m_device, m_pipeline, m_pipelineLayout, m_descriptorPool, m_setLayout);
    m_linearSampler = nullptr;
}
}
