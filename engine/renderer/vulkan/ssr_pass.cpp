#include "ssr_pass.h"

#include "nvrhi_pass.h"
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

// Must match SSR_TRACE_FLAG_* in shaders/vulkan/ssr_half_res.slang.
constexpr uint32_t kTraceFlagFullResolution = 1u;

// Must match SsrResolveConstants in shaders/vulkan/ssr_resolve.comp.
struct SsrResolvePushConstants
{
    glm::vec2 extent{0.0f};
    glm::vec2 invExtent{0.0f};
    float historyScale = 1.0f;
    uint32_t flags = 0;
    // The trace's, for which pixel of each 2x2 block it traced (ssr_half_res.slang).
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

nvrhi::BindingLayoutHandle CreateTraceSetLayout(nvrhi::IDevice* device, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(kTraceDepthBinding),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kTraceDepthBinding),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(kTraceHistoryBinding),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kTraceHistoryBinding),
        nvrhi::BindingLayoutItem::Texture_UAV(4),
        nvrhi::BindingLayoutItem::Texture_SRV(5),
        nvrhi::BindingLayoutItem::Texture_SRV(6),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SsrPushConstants))};
    return CreateNvrhiBindingLayout(device, desc, "Failed to create the SSR trace binding layout");
}

template <typename Constants>
void Dispatch(nvrhi::ICommandList* commandList, const nvrhi::ComputeState& state, const Constants& constants, VkExtent2D extent)
{
    commandList->setComputeState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
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
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    const VulkanTaaPass& taa,
    const VulkanRayScene& rayScene)
    : m_nvrhiDevice(nvrhiDevice),
      m_taa(taa)
{
    m_nearestSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
    m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    m_setLayout = CreateTraceSetLayout(m_nvrhiDevice, 1);
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ssr_trace.comp.spv", {frameSetLayout, m_setLayout});
    if (rayScene.HasHardwareRayTracing())
    {
        m_tracedSetLayout = CreateTraceSetLayout(m_nvrhiDevice, 2);
        m_tracedPipeline = CreateNvrhiComputePipeline(
            m_nvrhiDevice,
            "rt_reflection_trace.comp.spv",
            {frameSetLayout, rayScene.GetNvrhiSetLayout(), m_tracedSetLayout, rayScene.GetNvrhiTextureSetLayout()});
    }
    CreateBindingSets(targets);
}

VulkanSsrTracePass::~VulkanSsrTracePass() = default;

ScenePassId VulkanSsrTracePass::Id() const
{
    return ScenePassId::SsrTrace;
}

RenderPassIo VulkanSsrTracePass::Io() const
{
    // The coat target and the velocity target's coat normal: a coated pixel traces its coat's lobe
    // (ssr_lobe.slang).
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
    (void)commandBuffer;
    // Without valid history the TAA images may still be UNDEFINED; they are not touched.
    if (!SsrTraces(frame))
    {
        return;
    }

    SsrPushConstants constants{};
    constants.extent = Extent(frame);
    constants.invExtent = 1.0f / constants.extent;
    constants.maxDistance = std::clamp(frame.ssr.maxDistance, 1.0f, 200.0f);
    constants.maxRoughness = std::clamp(frame.ssr.maxRoughness, 0.05f, 1.0f);
    constants.historyScale = frame.taaHistoryScale;
    constants.frameIndex = frame.frameIndex;
    // DLSS ray reconstruction denoises the reflections itself and wants a raw sample in every pixel
    // (ssr_half_res.slang); otherwise half resolution, filtered by the resolve.
    constants.flags = frame.dlssRayReconstruction ? kTraceFlagFullResolution : 0u;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SsrRaw, frame.imageIndex, frame.frameSlot);
    const uint32_t setIndex = slot * 2 + frame.taaHistory.readIndex;
    const VkExtent2D extent = frame.dlssRayReconstruction ? frame.extent : VkExtent2D{(frame.extent.width + 1) / 2, (frame.extent.height + 1) / 2};
    // Last frame's TAA history rests in GENERAL, written by last frame's TAA resolve in an earlier
    // submission: the transition to the read layout makes that write visible, and the scope puts it
    // back. SsrRaw is written in GENERAL.
    nvrhi::ITexture* history = m_taa.GetHistoryTexture(frame.taaHistory.readIndex);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{history, nvrhi::ResourceStates::UnorderedAccess},
         {targets.GetTexture(RenderTargetId::SsrRaw, slot), nvrhi::ResourceStates::UnorderedAccess}});
    commandList->setTextureState(history, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    if (frame.rayTracing.reflections && m_tracedPipeline && frame.rayBindingSet != nullptr && frame.rayTextureTable != nullptr)
    {
        // The scene, not the screen: the rays reach as far as the sky.
        constants.maxDistance = kTracedReflectionDistance;
        state.pipeline = m_tracedPipeline;
        state.bindings = {frame.frameBindingSet, frame.rayBindingSet, m_tracedBindingSets.at(setIndex), frame.rayTextureTable};
    }
    else
    {
        state.pipeline = m_pipeline;
        state.bindings = {frame.frameBindingSet, m_bindingSets.at(setIndex)};
    }
    Dispatch(commandList, state, constants, extent);
}

void VulkanSsrTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateBindingSets(targets);
}

void VulkanSsrTracePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    m_tracedBindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        for (uint32_t historyIndex = 0; historyIndex < 2; ++historyIndex)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(kTraceDepthBinding, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
                nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + kTraceDepthBinding, m_nearestSampler),
                nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
                nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferSurface, slot)),
                nvrhi::BindingSetItem::Texture_SRV(kTraceHistoryBinding, m_taa.GetHistoryTexture(historyIndex)),
                nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + kTraceHistoryBinding, m_linearSampler),
                nvrhi::BindingSetItem::Texture_UAV(4, targets.GetTexture(RenderTargetId::SsrRaw, slot)),
                nvrhi::BindingSetItem::Texture_SRV(5, targets.GetTexture(RenderTargetId::GBufferCoat, slot)),
                nvrhi::BindingSetItem::Texture_SRV(6, targets.GetTexture(RenderTargetId::GBufferVelocity, slot)),
                nvrhi::BindingSetItem::PushConstants(0, sizeof(SsrPushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an SSR trace binding set"));
            if (m_tracedSetLayout)
            {
                m_tracedBindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_tracedSetLayout, "Failed to create an SSR trace binding set"));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanSsrResolvePass::VulkanSsrResolvePass(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    m_linearSampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    // The raw trace and the G-buffer loaded, the history sampled, the history and SceneReflections
    // written.
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(3),
        nvrhi::BindingLayoutItem::Texture_SRV(4),
        nvrhi::BindingLayoutItem::Texture_SRV(kResolveHistoryBinding),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset + kResolveHistoryBinding),
        nvrhi::BindingLayoutItem::Texture_UAV(6),
        nvrhi::BindingLayoutItem::Texture_UAV(7),
        nvrhi::BindingLayoutItem::Texture_SRV(8),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SsrResolvePushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the SSR resolve binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "ssr_resolve.comp.spv", {frameSetLayout, m_setLayout});
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

VulkanSsrResolvePass::~VulkanSsrResolvePass()
{
    m_bindingSets.clear();
    m_history.Destroy();
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

    SsrResolvePushConstants constants{};
    constants.extent = Extent(frame);
    constants.invExtent = 1.0f / constants.extent;
    // The resolve's history was written last frame, at last frame's pre-exposure, like TAA's.
    constants.historyScale = frame.taaHistoryScale;
    constants.flags = (SsrTraces(frame) ? kFlagTraced : 0u) | (frame.ssrHistory.valid ? kFlagHistoryValid : 0u) |
                      (frame.dlssRayReconstruction ? kFlagPassThrough : 0u);
    constants.frameIndex = frame.frameIndex;

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneReflections, frame.imageIndex, frame.frameSlot);
    nvrhi::ITexture* historyRead = m_history.GetTexture(frame.ssrHistory.readIndex);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{historyRead, nvrhi::ResourceStates::UnorderedAccess},
         {m_history.GetTexture(1u - frame.ssrHistory.readIndex), nvrhi::ResourceStates::UnorderedAccess},
         {targets.GetTexture(RenderTargetId::SceneReflections, slot), nvrhi::ResourceStates::UnorderedAccess}});
    commandList->setTextureState(historyRead, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_pipeline;
    state.bindings = {frame.frameBindingSet, m_bindingSets.at(slot * 2 + frame.ssrHistory.readIndex)};
    Dispatch(commandList, state, constants, frame.extent);
}

void VulkanSsrResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the SSR TemporalHistory at the same call sites, so the next frame discards
    // the new images' undefined contents.
    m_bindingSets.clear();
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

void VulkanSsrResolvePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SsrRaw, slot)),
                nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
                nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
                nvrhi::BindingSetItem::Texture_SRV(3, targets.GetTexture(RenderTargetId::GBufferSurface, slot)),
                nvrhi::BindingSetItem::Texture_SRV(4, targets.GetTexture(RenderTargetId::GBufferVelocity, slot)),
                nvrhi::BindingSetItem::Texture_SRV(kResolveHistoryBinding, m_history.GetTexture(readIndex)),
                nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset + kResolveHistoryBinding, m_linearSampler),
                nvrhi::BindingSetItem::Texture_UAV(6, m_history.GetTexture(1u - readIndex)),
                nvrhi::BindingSetItem::Texture_UAV(7, targets.GetTexture(RenderTargetId::SceneReflections, slot)),
                nvrhi::BindingSetItem::Texture_SRV(8, targets.GetTexture(RenderTargetId::GBufferCoat, slot)),
                nvrhi::BindingSetItem::PushConstants(0, sizeof(SsrResolvePushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an SSR resolve binding set"));
        }
    }
}
}
