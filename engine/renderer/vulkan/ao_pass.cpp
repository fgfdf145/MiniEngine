#include "ao_pass.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"
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

// Must match AoConstants in shaders/vulkan/vbao_common.slang.
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
static_assert(sizeof(AoPushConstants) == 48, "AoPushConstants must match vbao_common.slang");

// Must match the AO_FLAG_* constants in vbao_common.slang.
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

// The trace's set (vbao_trace.comp and rt_occlusion.comp): depth and normal loaded, AoRaw written, and
// the push constants, at descriptor set registerSpace.
nvrhi::BindingLayoutHandle CreateTraceSetLayout(nvrhi::IDevice* device, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_UAV(2),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AoPushConstants))};
    return CreateNvrhiBindingLayout(device, desc, "Failed to create the AO trace binding layout");
}

void Dispatch(nvrhi::ICommandList* commandList, const nvrhi::ComputeState& state, const AoPushConstants& constants, VkExtent2D extent)
{
    commandList->setComputeState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
}
}

// ---------------------------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------------------------

VulkanAoTracePass::VulkanAoTracePass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice)
{
    m_setLayout = CreateTraceSetLayout(m_nvrhiDevice, 1);
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "vbao_trace.comp.spv", {frameSetLayout, m_setLayout});
    if (rayScene.HasHardwareRayTracing())
    {
        m_tracedSetLayout = CreateTraceSetLayout(m_nvrhiDevice, 2);
        m_tracedPipeline = CreateNvrhiComputePipeline(
            m_nvrhiDevice,
            "rt_occlusion.comp.spv",
            {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_tracedSetLayout, rayScene.GetNvrhiTextureSetLayout()});
    }
    CreateBindingSets(targets);
}

VulkanAoTracePass::~VulkanAoTracePass() = default;

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
    (void)commandBuffer;
    const bool traced = m_tracedPipeline && frame.rayBindingSet != nullptr && frame.rayTextureTable != nullptr &&
                        (frame.rayTracing.ambientOcclusion || frame.rayTracing.probeOcclusion);
    const bool bitmask = frame.ao.enabled && !frame.rayTracing.ambientOcclusion;
    if (!traced && !bitmask)
    {
        return;
    }
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::AoRaw, frame.imageIndex, frame.frameSlot);
    const VkExtent2D extent = targets.GetTargetExtent(RenderTargetId::AoRaw);
    nvrhi::ITexture* aoRaw = targets.GetTexture(RenderTargetId::AoRaw, slot);
    nvrhi::ICommandList* commandList = frame.commandList;
    // The inputs are where the native passes left them, in the read layout; AoRaw is written in GENERAL.
    const NvrhiPassScope scope(commandList, {{aoRaw, nvrhi::ResourceStates::UnorderedAccess}});
    AoPushConstants constants = BuildPushConstants(frame);
    if (bitmask)
    {
        nvrhi::ComputeState state;
        state.pipeline = m_pipeline;
        state.bindings = {frame.frameBindingSet, m_bindingSets.at(slot)};
        Dispatch(commandList, state, constants, extent);
    }
    if (!traced)
    {
        return;
    }
    if (bitmask)
    {
        // The traced pass reads the bitmask's AO back and adds the probe occlusion beside it: a UAV
        // barrier between the two.
        commandList->setTextureState(aoRaw, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
    }
    // The traced rays per pixel ride in the slice count.
    constants.sliceCount = static_cast<uint32_t>(frame.rayTracing.occlusionRays);
    nvrhi::ComputeState state;
    state.pipeline = m_tracedPipeline;
    state.bindings = {frame.frameBindingSet, frame.rayBindingSet, m_tracedBindingSets.at(slot), frame.rayTextureTable};
    Dispatch(commandList, state, constants, extent);
}

void VulkanAoTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateBindingSets(targets);
}

void VulkanAoTracePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    m_tracedBindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
            nvrhi::BindingSetItem::Texture_UAV(2, targets.GetTexture(RenderTargetId::AoRaw, slot)),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(AoPushConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an AO trace binding set"));
        if (m_tracedSetLayout)
        {
            m_tracedBindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_tracedSetLayout, "Failed to create an AO trace binding set"));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanAoResolvePass::VulkanAoResolvePass(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    // The raw AO, depth and velocity loaded; the history sampled (bilinear reprojection, its sampler at
    // kHistoryReadBinding + 64); the history written and SceneAo.
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(kHistoryReadBinding),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kHistoryReadBinding),
        nvrhi::BindingLayoutItem::Texture_UAV(4),
        nvrhi::BindingLayoutItem::Texture_UAV(5),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AoPushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the AO resolve binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "vbao_resolve.comp.spv", {frameSetLayout, m_setLayout});
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

VulkanAoResolvePass::~VulkanAoResolvePass()
{
    m_bindingSets.clear();
    m_history.Destroy();
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
    // Runs even with AO off: SceneAo must hold something. Both history images go to GENERAL first
    // (discarded when the history is invalid), where they rest between frames.
    m_history.RecordBarrier(commandBuffer, frame.aoHistory.valid);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneAo, frame.imageIndex, frame.frameSlot);
    nvrhi::ITexture* historyRead = m_history.GetTexture(frame.aoHistory.readIndex);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{historyRead, nvrhi::ResourceStates::UnorderedAccess},
         {m_history.GetTexture(1u - frame.aoHistory.readIndex), nvrhi::ResourceStates::UnorderedAccess},
         {targets.GetTexture(RenderTargetId::SceneAo, slot), nvrhi::ResourceStates::UnorderedAccess}});
    // The history the dispatch samples, in the read layout for it; the scope puts it back in GENERAL.
    commandList->setTextureState(historyRead, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_pipeline;
    state.bindings = {frame.frameBindingSet, m_bindingSets.at(slot * 2 + frame.aoHistory.readIndex)};
    Dispatch(commandList, state, BuildPushConstants(frame), frame.extent);
}

void VulkanAoResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets TemporalHistory at the same call sites, so the next frame discards the new
    // images' undefined contents.
    m_bindingSets.clear();
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

void VulkanAoResolvePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::AoRaw, slot)),
                nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
                nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferVelocity, slot)),
                nvrhi::BindingSetItem::Texture_SRV(kHistoryReadBinding, m_history.GetTexture(readIndex)),
                nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + kHistoryReadBinding, m_linearSampler),
                nvrhi::BindingSetItem::Texture_UAV(4, m_history.GetTexture(1u - readIndex)),
                nvrhi::BindingSetItem::Texture_UAV(5, targets.GetTexture(RenderTargetId::SceneAo, slot)),
                nvrhi::BindingSetItem::PushConstants(0, sizeof(AoPushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an AO resolve binding set"));
        }
    }
}
}
