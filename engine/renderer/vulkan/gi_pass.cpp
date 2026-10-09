#include "gi_pass.h"

#include "compute_pass_util.h"
#include "gbuffer_inputs.h"
#include "nvrhi_pass.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
constexpr float kMaxPixelRadius = 256.0f;
// rgb plus the packed distance and sample count, which half floats cannot hold exactly.
constexpr VkFormat kHistoryFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// Must match AoConstants in shaders/vulkan/vbao_common.slang, which the GI shaders share.
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
static_assert(sizeof(GiPushConstants) == 48, "GiPushConstants must match vbao_common.slang");

// Must match the AO_FLAG_* constants in vbao_common.slang.
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

// Set 1 of the trace and the resolve: every input read with Load (no samplers), then the images
// written.
nvrhi::BindingLayoutHandle CreateSetLayout(nvrhi::IDevice* device, uint32_t textureCount, uint32_t imageCount, const char* failure)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    for (uint32_t binding = 0; binding < textureCount; ++binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(binding));
    }
    for (uint32_t binding = textureCount; binding < textureCount + imageCount; ++binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(binding));
    }
    desc.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GiPushConstants)));
    return CreateNvrhiBindingLayout(device, desc, failure);
}

void Dispatch(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::IBindingSet* frameSet, nvrhi::IBindingSet* set, const GiPushConstants& constants, VkExtent2D extent)
{
    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings = {frameSet, set};
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

VulkanGiTracePass::VulkanGiTracePass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    m_setLayout = CreateSetLayout(m_nvrhiDevice, 3, 1, "Failed to create the GI trace binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "gi_trace.comp.spv", {frameSetLayout, m_setLayout});
    CreateBindingSets(targets);
}

VulkanGiTracePass::~VulkanGiTracePass() = default;

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
    (void)commandBuffer;
    if (!frame.gi.enabled)
    {
        return;
    }
    // The inputs are where the native passes left them, in the read layout; GiRaw goes back to GENERAL.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::GiRaw, frame.imageIndex, frame.frameSlot);
    const NvrhiPassScope scope(frame.commandList, {{targets.GetTexture(RenderTargetId::GiRaw, slot), nvrhi::ResourceStates::UnorderedAccess}});
    Dispatch(
        frame.commandList,
        m_pipeline,
        frame.frameBindingSet,
        m_bindingSets.at(slot),
        BuildPushConstants(frame),
        targets.GetTargetExtent(RenderTargetId::GiRaw));
}

void VulkanGiTracePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateBindingSets(targets);
}

void VulkanGiTracePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
            nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
            nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::SceneHdr, slot)),
            nvrhi::BindingSetItem::Texture_UAV(3, targets.GetTexture(RenderTargetId::GiRaw, slot)),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(GiPushConstants))};
        m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a GI trace binding set"));
    }
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

VulkanGiResolvePass::VulkanGiResolvePass(nvrhi::IDevice* nvrhiDevice, VkDevice device, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_nvrhiDevice(nvrhiDevice),
      m_device(device)
{
    // The history is read as a storage image too: read through a shader resource view, in
    // SHADER_READ_ONLY_OPTIMAL, the RGBA32F history gave other results on NVIDIA than read in GENERAL
    // (the GI view about 9% darker; docs/design/2026-10-08-nvrhi-backend-design.md).
    m_setLayout = CreateSetLayout(m_nvrhiDevice, 4, 3, "Failed to create the GI resolve binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "gi_resolve.comp.spv", {frameSetLayout, m_setLayout});
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

VulkanGiResolvePass::~VulkanGiResolvePass()
{
    m_history.Destroy();
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
    // Runs even with GI off: the debug view reads the zero it writes. Both history images are put in
    // GENERAL first (discarded when the history is invalid), and stay there.

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneGi, frame.imageIndex, frame.frameSlot);
    nvrhi::ITexture* historyRead = m_history.GetTexture(frame.giHistory.readIndex);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(
        commandList,
        {{historyRead, nvrhi::ResourceStates::UnorderedAccess},
         {m_history.GetTexture(1u - frame.giHistory.readIndex), nvrhi::ResourceStates::UnorderedAccess},
         {targets.GetTexture(RenderTargetId::SceneGi, slot), nvrhi::ResourceStates::UnorderedAccess}});
    Dispatch(commandList, m_pipeline, frame.frameBindingSet, m_bindingSets.at(slot * 2 + frame.giHistory.readIndex), BuildPushConstants(frame), frame.extent);
}

void VulkanGiResolvePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The renderer resets the GI history at the same call sites, as it does the AO's.
    m_bindingSets.clear();
    m_history.Create(m_nvrhiDevice, m_device, targets.GetExtent(), kHistoryFormat);
    CreateBindingSets(targets);
}

void VulkanGiResolvePass::CreateBindingSets(const SceneRenderTargets& targets)
{
    m_bindingSets.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        for (uint32_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {
                nvrhi::BindingSetItem::Texture_SRV(0, targets.GetTexture(RenderTargetId::GiRaw, slot)),
                nvrhi::BindingSetItem::Texture_SRV(1, targets.GetTexture(RenderTargetId::SceneDepth, slot)),
                nvrhi::BindingSetItem::Texture_SRV(2, targets.GetTexture(RenderTargetId::GBufferVelocity, slot)),
                nvrhi::BindingSetItem::Texture_SRV(3, targets.GetTexture(RenderTargetId::GBufferNormal, slot)),
                nvrhi::BindingSetItem::Texture_UAV(4, m_history.GetTexture(readIndex)),
                nvrhi::BindingSetItem::Texture_UAV(5, m_history.GetTexture(1u - readIndex)),
                nvrhi::BindingSetItem::Texture_UAV(6, targets.GetTexture(RenderTargetId::SceneGi, slot)),
                nvrhi::BindingSetItem::PushConstants(0, sizeof(GiPushConstants))};
            m_bindingSets.push_back(CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a GI resolve binding set"));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------------------------

VulkanGiCompositePass::VulkanGiCompositePass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    nvrhi::IBindingLayout* gbufferSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    CreateFramebuffers(targets);
    nvrhi::GraphicsPipelineDesc desc;
    desc.VS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Vertex, "fullscreen.vert.spv");
    desc.PS = CreateNvrhiShader(m_nvrhiDevice, nvrhi::ShaderType::Pixel, "gi_composite.frag.spv");
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.bindingLayouts = {frameSetLayout, gbufferSetLayout};
    desc.renderState.rasterState.setCullNone();
    desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
    // ONE + ONE on rgb, the attachment's alpha kept: the bounce is added to the lit image.
    nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
    blend.enableBlend()
        .setSrcBlend(nvrhi::BlendFactor::One)
        .setDestBlend(nvrhi::BlendFactor::One)
        .setBlendOp(nvrhi::BlendOp::Add)
        .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
        .setDestBlendAlpha(nvrhi::BlendFactor::One)
        .setBlendOpAlpha(nvrhi::BlendOp::Add);
    m_pipeline = m_nvrhiDevice->createGraphicsPipeline(desc, m_framebuffers.front());
    if (!m_pipeline)
    {
        throw std::runtime_error("Failed to create the GI composite pipeline");
    }
}

VulkanGiCompositePass::~VulkanGiCompositePass() = default;

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
    (void)commandBuffer;
    if (!frame.gi.enabled)
    {
        return;
    }
    // The layout tracker put the HDR target in COLOR_ATTACHMENT_OPTIMAL (the write) and the G-buffer in
    // the read layout; the scope's end closes the rendering NVRHI began.
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SceneHdr, slot), nvrhi::ResourceStates::RenderTarget}});
    nvrhi::GraphicsState state;
    state.pipeline = m_pipeline;
    state.framebuffer = m_framebuffers.at(slot);
    state.viewport = NativeViewportState(frame.extent);
    state.bindings = {frame.frameBindingSet, frame.gbufferBindingSet};
    commandList->setGraphicsState(state);
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void VulkanGiCompositePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
}

void VulkanGiCompositePass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::FramebufferHandle framebuffer = m_nvrhiDevice->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(targets.GetTexture(RenderTargetId::SceneHdr, slot)));
        if (!framebuffer)
        {
            throw std::runtime_error("Failed to create a GI composite framebuffer");
        }
        m_framebuffers.push_back(framebuffer);
    }
}
}
