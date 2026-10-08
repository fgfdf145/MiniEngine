#include "gi_pass.h"

#include "compute_pass_util.h"
#include "gbuffer_inputs.h"
#include "pipeline.h"

#include <algorithm>
#include <array>
#include <vector>

namespace me
{

namespace
{
constexpr float kMaxPixelRadius = 256.0f;
// rgb plus the packed distance and sample count, which half floats cannot hold exactly.
constexpr VkFormat kHistoryFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// Must match AoConstants in shaders/vulkan/vbao_common.glsl, which the GI shaders share.
struct GiPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float radius = 0.0f;
    float thickness = 0.0f;
    float maxPixelRadius = 0.0f;
    float strength = 0.0f;
    uint32_t sliceCount = 0;
    uint32_t stepCount = 0;
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(GiPushConstants) == 48, "GiPushConstants must match vbao_common.glsl");

// Must match the AO_FLAG_* constants in vbao_common.glsl.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagSpatial = 2u;
constexpr uint32_t kFlagTemporal = 4u;
constexpr uint32_t kFlagHistoryValid = 8u;

// Clamps every setting to the range the editor offers, as the AO pass does.
GiPushConstants BuildPushConstants(const ScenePassFrameContext& frame)
{
    GiPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.radius = std::clamp(frame.gi.radius, 0.25f, 10.0f);
    constants.thickness = std::clamp(frame.gi.thickness, 0.01f, 2.0f);
    constants.maxPixelRadius = kMaxPixelRadius;
    constants.strength = std::clamp(frame.gi.strength, 0.0f, 4.0f);
    constants.sliceCount = static_cast<uint32_t>(std::clamp(frame.gi.sliceCount, 1, 4));
    constants.stepCount = static_cast<uint32_t>(std::clamp(frame.gi.stepCount, 2, 32));
    // Offset from the AO's so the two noise patterns decorrelate over time as well.
    constants.frameIndex = frame.frameIndex + 17u;
    constants.flags =
        (frame.gi.enabled ? kFlagEnabled : 0u) |
        (frame.gi.spatialFilter ? kFlagSpatial : 0u) |
        (frame.gi.temporalFilter ? kFlagTemporal : 0u) |
        (frame.giHistory.valid ? kFlagHistoryValid : 0u);
    return constants;
}

void DestroyPipelineHandles(VkDevice device, VkPipeline& pipeline, VkPipelineLayout& layout)
{
    if (pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
}

void DestroySetHandles(VkDevice device, VkDescriptorPool& pool, VkDescriptorSetLayout& layout, nvrhi::SamplerHandle& sampler)
{
    if (pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    if (layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        layout = VK_NULL_HANDLE;
    }
    sampler = nullptr;
}
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanGiTracePass::VulkanGiTracePass(
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_device(device)
{
    try
    {
        m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
        static constexpr std::array<VkDescriptorType, 4> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "gi_trace.comp.spv", sizeof(GiPushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount(), 3, 1);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanGiTracePass::~VulkanGiTracePass()
{
    DestroyHandles();
}

ScenePassId VulkanGiTracePass::Id() const
{
    return ScenePassId::GiTrace;
}

RenderPassIo VulkanGiTracePass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kReads = {
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferNormal,
        RenderTargetId::SceneHdr};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::GiRaw};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanGiTracePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.gi.enabled)
    {
        return;
    }
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::GiRaw, frame.imageIndex, frame.frameSlot);
    const GiPushConstants constants = BuildPushConstants(frame);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot),
        &constants,
        sizeof(constants),
        targets.GetTargetExtent(RenderTargetId::GiRaw));
}

void VulkanGiTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanGiTracePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkDescriptorImageInfo depthInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
        const VkDescriptorImageInfo normalInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
        const VkDescriptorImageInfo hdrInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::SceneHdr, slot), kReadLayout};
        const VkDescriptorImageInfo giInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::GiRaw, slot), VK_IMAGE_LAYOUT_GENERAL};
        const std::array<VkWriteDescriptorSet, 4> writes = {
            ImageWrite(m_descriptorSets[slot], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
            ImageWrite(m_descriptorSets[slot], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
            ImageWrite(m_descriptorSets[slot], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrInfo),
            ImageWrite(m_descriptorSets[slot], 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &giInfo)};
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanGiTracePass::DestroyHandles()
{
    DestroyPipelineHandles(m_device, m_pipeline, m_pipelineLayout);
    m_descriptorSets.clear();
    DestroySetHandles(m_device, m_descriptorPool, m_setLayout, m_sampler);
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanGiResolvePass::VulkanGiResolvePass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
        static constexpr std::array<VkDescriptorType, 7> kTypes = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        m_setLayout = CreateComputeSetLayout(m_device, kTypes);
        CreateComputePipeline(m_device, pipelineCache, frameSetLayout, m_setLayout, "gi_resolve.comp.spv", sizeof(GiPushConstants), m_pipelineLayout, m_pipeline);
        m_descriptorPool = CreateImageDescriptorPool(m_device, targets.GetTransientCopyCount() * 2, 5, 2);
        m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanGiResolvePass::~VulkanGiResolvePass()
{
    DestroyHandles();
}

ScenePassId VulkanGiResolvePass::Id() const
{
    return ScenePassId::GiResolve;
}

RenderPassIo VulkanGiResolvePass::Io() const
{
    static constexpr std::array<RenderTargetId, 4> kReads = {
        RenderTargetId::GiRaw,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::GBufferNormal};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneGi};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanGiResolvePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // In path tracing mode SceneGi holds the path traced diffuse light, which stays for the indirect
    // diffuse view and the reference comparison (--reference); nothing binds this pass's history. ReSTIR
    // PT writes elsewhere, and GI is off then, so this writes the zero.
    if (frame.pathTracing.enabled && !frame.pathTracing.restir)
    {
        return;
    }
    // Runs even with GI off: the bound descriptors name both history images in GENERAL, and the
    // debug view reads the zero it writes.
    m_history.RecordBarrier(commandBuffer, frame.giHistory.valid);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
    const GiPushConstants constants = BuildPushConstants(frame);
    DispatchCompute(
        commandBuffer,
        m_pipeline,
        m_pipelineLayout,
        frame.frameDescriptorSet,
        m_descriptorSets.at(slot * 2 + frame.giHistory.readIndex),
        &constants,
        sizeof(constants),
        frame.extent);
}

void VulkanGiResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the GI history at the same call sites, as it does the AO's.
    m_history.Create(m_physicalDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateDescriptorSets(targets);
}

void VulkanGiResolvePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_descriptorSets = AllocateDescriptorSets(m_device, m_descriptorPool, m_setLayout, copyCount * 2);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const VkDescriptorSet set = m_descriptorSets[slot * 2 + readIndex];
            const VkDescriptorImageInfo rawInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::GiRaw, slot), kReadLayout};
            const VkDescriptorImageInfo depthInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::SceneDepth, slot), kReadLayout};
            const VkDescriptorImageInfo velocityInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::GBufferVelocity, slot), kReadLayout};
            const VkDescriptorImageInfo normalInfo{NativeSampler(m_sampler), targets.GetSampledView(RenderTargetId::GBufferNormal, slot), kReadLayout};
            const VkDescriptorImageInfo historyReadInfo{NativeSampler(m_sampler), m_history.GetView(readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo historyWriteInfo{VK_NULL_HANDLE, m_history.GetView(1u - readIndex), VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo giInfo{VK_NULL_HANDLE, targets.GetView(RenderTargetId::SceneGi, slot), VK_IMAGE_LAYOUT_GENERAL};
            const std::array<VkWriteDescriptorSet, 7> writes = {
                ImageWrite(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rawInfo),
                ImageWrite(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo),
                ImageWrite(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &velocityInfo),
                ImageWrite(set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo),
                ImageWrite(set, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &historyReadInfo),
                ImageWrite(set, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &historyWriteInfo),
                ImageWrite(set, 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &giInfo)};
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void VulkanGiResolvePass::DestroyHandles()
{
    DestroyPipelineHandles(m_device, m_pipeline, m_pipelineLayout);
    m_descriptorSets.clear();
    m_history.Destroy();
    DestroySetHandles(m_device, m_descriptorPool, m_setLayout, m_sampler);
}

// ---------------------------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------------------------

VulkanGiCompositePass::VulkanGiCompositePass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout emptySetLayout,
    VkDescriptorSetLayout gbufferSetLayout)
    : m_device(device)
{
    try
    {
        // LOAD: the lit image is what the bounce is added to.
        m_renderPass = CreateFullscreenRenderPass(m_device, targets.GetFormat(RenderTargetId::SceneHdr), "GI composite", VK_ATTACHMENT_LOAD_OP_LOAD);

        const std::array<VkDescriptorSetLayout, 3> setLayouts = {frameSetLayout, emptySetLayout, gbufferSetLayout};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create GI composite pipeline layout");

        FullscreenPipelineOptions options{};
        options.additiveBlend = true;
        m_pipeline = CreateFullscreenPipeline(m_device, pipelineCache, m_renderPass, m_pipelineLayout, "gi_composite.frag.spv", "GI composite", options);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanGiCompositePass::~VulkanGiCompositePass()
{
    DestroyHandles();
}

ScenePassId VulkanGiCompositePass::Id() const
{
    return ScenePassId::GiComposite;
}

RenderPassIo VulkanGiCompositePass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneHdr};
    RenderPassIo io{};
    io.reads = VulkanGBufferDescriptors::kInputs;
    io.writes = kWrites;
    return io;
}

void VulkanGiCompositePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (!frame.gi.enabled)
    {
        return;
    }
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = frame.extent;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &frame.frameDescriptorSet, 0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 2, 1, &frame.gbufferDescriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanGiCompositePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

void VulkanGiCompositePass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkImageView attachment = targets.GetView(RenderTargetId::SceneHdr, slot);
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create GI composite framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanGiCompositePass::DestroyFramebuffers()
{
    for (VkFramebuffer framebuffer : m_framebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_framebuffers.clear();
}

void VulkanGiCompositePass::DestroyHandles()
{
    DestroyPipelineHandles(m_device, m_pipeline, m_pipelineLayout);
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
