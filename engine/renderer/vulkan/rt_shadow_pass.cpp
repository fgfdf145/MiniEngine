#include "rt_shadow_pass.h"

#include "nvrhi_pass.h"
#include "ray_scene.h"

#include <array>

namespace me
{

namespace
{
// Must match RtShadowConstants in shaders/vulkan/rt_shadow_common.slang.
struct RtShadowPushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
    float unused0 = 0.0f;
    float unused1 = 0.0f;
};
static_assert(sizeof(RtShadowPushConstants) == 32, "RtShadowPushConstants must match rt_shadow_common.slang");

// Must match the RT_SHADOW_FLAG_* constants in rt_shadow_common.slang.
constexpr uint32_t kFlagEnabled = 1u;
constexpr uint32_t kFlagDenoise = 2u;
constexpr uint32_t kFlagHistoryValid = 4u;

constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// rt_shadow_common.slang's rtShadowHistoryRead, the one input the shaders sample rather than load.
constexpr uint32_t kHistoryReadBinding = 4;

nvrhi::BindingLayoutHandle CreateSetLayout(nvrhi::IDevice* device, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_UAV(3),
        nvrhi::BindingLayoutItem::Texture_SRV(kHistoryReadBinding),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kHistoryReadBinding),
        nvrhi::BindingLayoutItem::Texture_UAV(5),
        nvrhi::BindingLayoutItem::Texture_UAV(6),
        nvrhi::BindingLayoutItem::Texture_UAV(7),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RtShadowPushConstants))};
    return CreateNvrhiBindingLayout(device, desc, "Failed to create the traced shadow binding layout");
}

void Dispatch(nvrhi::ICommandList* commandList, const nvrhi::ComputeState& state, const RtShadowPushConstants& constants, VkExtent2D extent)
{
    commandList->setComputeState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
}

// What one dispatch wrote, visible to the next: a UAV barrier on each image written.
void UavBarrier(nvrhi::ICommandList* commandList, std::initializer_list<nvrhi::ITexture*> textures)
{
    for (nvrhi::ITexture* texture : textures)
    {
        commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->commitBarriers();
}
}

VulkanRtShadowPass::VulkanRtShadowPass(
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    m_setLayout = CreateSetLayout(m_nvrhiDevice, 1);
    m_filterPipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "rt_shadow_filter.comp.spv", {frameSetLayout, m_setLayout});
    if (rayScene.HasHardwareRayTracing())
    {
        m_temporalPipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "rt_shadow_temporal.comp.spv", {frameSetLayout, m_setLayout});
        m_traceSetLayout = CreateSetLayout(m_nvrhiDevice, 2);
        m_tracePipeline = CreateNvrhiComputePipeline(
            m_nvrhiDevice,
            "rt_shadow_trace.comp.spv",
            {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_traceSetLayout, rayScene.GetNvrhiTextureSetLayout()});
    }
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

VulkanRtShadowPass::~VulkanRtShadowPass()
{
    m_traceBindingSets.clear();
    m_bindingSets.clear();
    m_history.Destroy();
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
    // Even while nothing traces: both history images rest in GENERAL.
    const bool traced = frame.rayTracing.sunShadows && m_tracePipeline && frame.rayBindingSet != nullptr && frame.rayTextureTable != nullptr;
    m_history.RecordBarrier(commandBuffer, traced && frame.rtShadowHistory.valid);

    RtShadowPushConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.frameIndex = frame.frameIndex;
    constants.flags = (traced ? kFlagEnabled : 0u) | (frame.rayTracing.denoise ? kFlagDenoise : 0u) |
                      (frame.rtShadowHistory.valid ? kFlagHistoryValid : 0u);

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneShadow, frame.imageIndex, frame.frameSlot);
    const uint32_t setIndex = slot * 2 + frame.rtShadowHistory.readIndex;
    nvrhi::ITexture* raw = targets.GetTexture(RenderTargetId::ShadowRaw, slot);
    nvrhi::ITexture* historyRead = m_history.GetTexture(frame.rtShadowHistory.readIndex);
    nvrhi::ITexture* historyWrite = m_history.GetTexture(1u - frame.rtShadowHistory.readIndex);
    nvrhi::ITexture* shadow = targets.GetTexture(RenderTargetId::SceneShadow, slot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{raw, nvrhi::ResourceStates::UnorderedAccess},
         {historyRead, nvrhi::ResourceStates::UnorderedAccess},
         {historyWrite, nvrhi::ResourceStates::UnorderedAccess},
         {shadow, nvrhi::ResourceStates::UnorderedAccess}});
    // The history the temporal pass samples, in the read layout for it (the scope puts it back).
    commandList->setTextureState(historyRead, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    if (traced)
    {
        nvrhi::ComputeState state;
        state.pipeline = m_tracePipeline;
        state.bindings = {frame.frameBindingSet, frame.rayBindingSet, m_traceBindingSets.at(setIndex), frame.rayTextureTable};
        Dispatch(commandList, state, constants, frame.extent);
        UavBarrier(commandList, {raw});
    }

    nvrhi::ComputeState state;
    state.bindings = {frame.frameBindingSet, m_bindingSets.at(setIndex)};
    if (traced && frame.rayTracing.denoise)
    {
        state.pipeline = m_temporalPipeline;
        Dispatch(commandList, state, constants, frame.extent);
        UavBarrier(commandList, {historyWrite});
    }
    state.pipeline = m_filterPipeline;
    Dispatch(commandList, state, constants, frame.extent);
}

void VulkanRtShadowPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the history bookkeeping at the same call sites.
    m_traceBindingSets.clear();
    m_bindingSets.clear();
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

void VulkanRtShadowPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_traceBindingSets.clear();
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            nvrhi::ITexture* historyWrite = m_history.GetTexture(1u - readIndex);
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
                nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
                nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferVelocity, slot)),
                nvrhi::BindingSetItem::Texture_UAV(3, targets.GetTexture(RenderTargetId::ShadowRaw, slot)),
                nvrhi::BindingSetItem::Texture_SRV(kHistoryReadBinding, m_history.GetTexture(readIndex)),
                nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + kHistoryReadBinding, m_linearSampler),
                nvrhi::BindingSetItem::Texture_UAV(5, historyWrite),
                nvrhi::BindingSetItem::Texture_UAV(6, targets.GetTexture(RenderTargetId::SceneShadow, slot)),
                nvrhi::BindingSetItem::Texture_UAV(7, historyWrite),
                nvrhi::BindingSetItem::PushConstants(0, sizeof(RtShadowPushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a traced shadow binding set"));
            if (m_traceSetLayout)
            {
                m_traceBindingSets.push_back(
                    CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_traceSetLayout, "Failed to create a traced shadow binding set"));
            }
        }
    }
}
}
