#include "forward_pass.h"

#include "command.h"
#include "material_draw.h"
#include "reverse_depth.h"

#include <engine/renderer/camera.h>
#include <engine/renderer/exposure.h>
#include <engine/renderer/material.h>

#include <array>
#include <stdexcept>

namespace me
{

namespace
{
constexpr float kKhronosViewerBackgroundRoughness = 0.6f;
}

VulkanForwardPass::VulkanForwardPass(
    nvrhi::IDevice* nvrhiDevice,
    const SceneRenderTargets& targets,
    nvrhi::IBindingLayout* frameSetLayout,
    ForwardPassPart part)
    : m_nvrhiDevice(nvrhiDevice),
      m_part(part)
{
    CreateFramebuffers(targets);
    m_skyConstants = CreatePushConstantLayout(m_nvrhiDevice, 1, sizeof(glm::vec4), nvrhi::ShaderType::Pixel);
    FullscreenNvrhiOptions skyOptions{};
    skyOptions.vertexShader = "sky.vert.spv";
    skyOptions.depthTestAtFarPlane = true;
    m_skyPipeline = CreateFullscreenNvrhiPipeline(m_nvrhiDevice, GetFramebufferInfo(), {frameSetLayout, m_skyConstants.layout}, "sky.frag.spv", skyOptions);
}

VulkanForwardPass::~VulkanForwardPass() = default;

ScenePassId VulkanForwardPass::Id() const
{
    return m_part == ForwardPassPart::OpaqueAndSky ? ScenePassId::Forward : ScenePassId::ForwardTranslucent;
}

RenderPassIo VulkanForwardPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kWrites = {
        RenderTargetId::SceneHdr,
        RenderTargetId::SceneDepth};

    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanForwardPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    nvrhi::ITexture* hdr = targets.GetTexture(RenderTargetId::SceneHdr, slot);
    nvrhi::ITexture* depth = targets.GetTexture(RenderTargetId::SceneDepth, slot);
    // The layout tracker put both in their attachment layouts.
    const NvrhiPassScope scope(
        commandList, {{hdr, nvrhi::ResourceStates::RenderTarget}, {depth, nvrhi::ResourceStates::DepthWrite}});

    // Only the opaque half ever clears: the translucent half always draws over it.
    const bool ownsFrame = m_part == ForwardPassPart::OpaqueAndSky && frame.forwardFilter == ForwardDrawFilter::All;
    if (ownsFrame)
    {
        // The clear lands in the HDR target and is tone mapped with everything else. It stands for no
        // physical light, so it is written as is at every exposure; the lighting pass writes the same
        // value for background pixels in the deferred order.
        const glm::vec3 background = kViewportBackgroundFrameBuffer;
        ClearTextureFloat(commandList, hdr, nvrhi::Color(background.r, background.g, background.b, 1.0f));
        ClearDepth(commandList, depth, kReverseDepthFar);
        commandList->setTextureState(hdr, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
        commandList->setTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
        commandList->commitBarriers();
    }

    MaterialDrawTarget target;
    target.framebuffer = m_framebuffers.at(slot);
    target.viewport = NativeViewportState(frame.extent);
    target.frameSet = frame.frameBindingSet;
    target.drawConstantsSet = frame.drawConstantsSet;
    if (m_part == ForwardPassPart::OpaqueAndSky)
    {
        // Opaque and Mask first: all of them when this pass owns the frame; otherwise the lighting
        // pass shaded all but the forward-shaded ones, which land here on the depth the geometry
        // pass wrote for them. Then the sky into whatever no geometry covered.
        RecordMaterialPass(
            commandList,
            frame.recorder,
            *frame.forwardPipelines,
            target,
            ownsFrame ? frame.OpaqueDrawItems() : frame.ForwardShadedDrawItems(),
            [&](nvrhi::ICommandList* tailList)
            {
                RecordSky(tailList, target.framebuffer, frame);
            });
        return;
    }

    // Over the transmission copy of the above: transmissive items, then Blend over everything. Few
    // draws, back to front; recorded inline.
    RecordMaterialDrawItems(commandList, *frame.forwardPipelines, target, frame.TransmissiveDrawItems());
    RecordMaterialDrawItems(commandList, *frame.forwardPipelines, target, frame.BlendDrawItems());
}

void VulkanForwardPass::RecordSky(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const ScenePassFrameContext& frame) const
{
    nvrhi::GraphicsState state;
    state.pipeline = m_skyPipeline;
    state.framebuffer = framebuffer;
    state.viewport = NativeViewportState(frame.extent);
    state.bindings = {frame.frameBindingSet, m_skyConstants.set};
    commandList->setGraphicsState(state);
    // w: the roughness whose prefiltered environment the HDRI background shows, 0 for the map
    // itself. The Khronos reference view blurs it as the Sample Viewer does (blurEnvironmentMap, 0.6).
    const glm::vec4 background(kViewportBackgroundFrameBuffer, frame.khronosReference ? kKhronosViewerBackgroundRoughness : 0.0f);
    commandList->setPushConstants(&background, sizeof(background));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void VulkanForwardPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
}

const nvrhi::FramebufferInfo& VulkanForwardPass::GetFramebufferInfo() const
{
    return m_framebuffers.front()->getFramebufferInfo();
}

void VulkanForwardPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        m_framebuffers.push_back(CreateNvrhiFramebuffer(
            m_nvrhiDevice, {targets.GetTexture(RenderTargetId::SceneHdr, slot)}, targets.GetTexture(RenderTargetId::SceneDepth, slot)));
    }
}
}
