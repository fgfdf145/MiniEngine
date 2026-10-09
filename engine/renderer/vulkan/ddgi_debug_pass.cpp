#include "ddgi_debug_pass.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "ray_scene.h"

#include <array>

namespace me
{

namespace
{
// Must match DdgiDebugConstants in shaders/vulkan/ddgi_debug.comp.
struct DdgiDebugConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    uint32_t view = 0;
    uint32_t frameIndex = 0;
};

bool IsDdgiDebugView(GBufferDebugView view)
{
    return view == GBufferDebugView::RayTraced || view == GBufferDebugView::DdgiIrradiance || view == GBufferDebugView::DdgiProbes;
}
}

VulkanDdgiDebugPass::VulkanDdgiDebugPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice),
      m_rayScene(rayScene)
{
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
    // SceneGi written; depth, normal and HDR colour sampled (nearest) at the view's UVs.
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 2;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_UAV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(3),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + 1),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + 2),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + 3),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DdgiDebugConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the DDGI debug binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ddgi_debug.comp.spv", {frameSetLayout, m_rayScene.GetNvrhiSetLayout(), m_setLayout});
    if (m_rayScene.HasHardwareRayTracing())
    {
        m_rayQueryPipeline = CreateNvrhiComputePipeline(
            m_nvrhiDevice, "ddgi_debug_ray_query.comp.spv", {frameSetLayout, m_rayScene.GetNvrhiSetLayout(), m_setLayout});
    }
    CreateBindingSets(targets);
}

VulkanDdgiDebugPass::~VulkanDdgiDebugPass() = default;

ScenePassId VulkanDdgiDebugPass::Id() const
{
    return ScenePassId::DdgiDebug;
}

RenderPassIo VulkanDdgiDebugPass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kReads = {RenderTargetId::SceneDepth, RenderTargetId::GBufferNormal, RenderTargetId::SceneHdr};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneGi};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanDdgiDebugPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    if (!IsDdgiDebugView(frame.gbufferView) || !m_rayScene.IsReady())
    {
        return;
    }
    DdgiDebugConstants constants{};
    constants.extent = glm::vec2(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
    constants.invExtent = 1.0f / constants.extent;
    constants.view = static_cast<uint32_t>(frame.gbufferView);
    constants.frameIndex = frame.frameIndex;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    // The inputs are in the read layout the native passes left them in; SceneGi is written in GENERAL.
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SceneGi, slot), nvrhi::ResourceStates::UnorderedAccess}});
    const bool rayQuery = frame.hardwareRays && m_rayQueryPipeline;
    nvrhi::ComputeState state;
    state.pipeline = rayQuery ? m_rayQueryPipeline : m_pipeline;
    state.bindings = {frame.frameBindingSet, m_rayScene.GetBindingSet(frame.frameSlot), m_bindingSets.at(slot)};
    commandList->setComputeState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(
        (frame.extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (frame.extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
}

void VulkanDdgiDebugPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateBindingSets(targets);
}

void VulkanDdgiDebugPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_UAV(0, targets.GetTexture(RenderTargetId::SceneGi, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
            nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
            nvrhi::BindingSetItem::Texture_SRV(3, targets.GetTexture(RenderTargetId::SceneHdr, slot)),
            nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + 1, m_sampler),
            nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + 2, m_sampler),
            nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + 3, m_sampler),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(DdgiDebugConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a DDGI debug binding set"));
    }
}
}
