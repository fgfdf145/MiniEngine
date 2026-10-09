#include "geometry_pass.h"

#include "material_draw.h"
#include "nvrhi_pass.h"
#include "reverse_depth.h"

#include <vector>

namespace me
{

VulkanGeometryPass::VulkanGeometryPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    CreateFramebuffers(targets);
    FullscreenNvrhiOptions groundOptions{};
    groundOptions.depthTestAndWrite = true;
    groundOptions.colorAttachmentCount = kColorAttachmentCount;
    m_groundPipeline = CreateFullscreenNvrhiPipeline(m_nvrhiDevice, GetFramebufferInfo(), {frameSetLayout}, "ground.frag.spv", groundOptions);
}

VulkanGeometryPass::~VulkanGeometryPass() = default;

ScenePassId VulkanGeometryPass::Id() const
{
    return ScenePassId::Geometry;
}

RenderPassIo VulkanGeometryPass::Io() const
{
    RenderPassIo io{};
    io.writes = kAttachments;
    return io;
}

void VulkanGeometryPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::GBufferAlbedo, frame.imageIndex, frame.frameSlot);
    nvrhi::ICommandList* commandList = frame.commandList;
    // The layout tracker put every target in its write layout.
    std::vector<NvrhiSharedTexture> shared;
    for (const RenderTargetId target : kAttachments)
    {
        shared.push_back(NvrhiSharedTexture{
            targets.GetTexture(target, slot),
            target == RenderTargetId::SceneDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget});
    }
    const NvrhiPassScope scope(commandList, shared);
    // The lighting pass identifies background pixels by the far plane's depth (reverse-Z: 0), never by
    // these colour clears, so zero serves; it also makes unwritten G-buffer pixels read as black in the
    // debug views.
    for (uint32_t index = 0; index < kColorAttachmentCount; ++index)
    {
        ClearTextureFloat(commandList, shared[index].texture, nvrhi::Color(0.0f));
    }
    ClearDepth(commandList, shared[kColorAttachmentCount].texture, kReverseDepthFar);
    for (const NvrhiSharedTexture& target : shared)
    {
        commandList->setTextureState(target.texture, nvrhi::AllSubresources, target.state);
    }
    commandList->commitBarriers();

    MaterialDrawTarget target;
    target.framebuffer = m_framebuffers.at(slot);
    target.viewport = NativeViewportState(frame.extent);
    target.frameSet = frame.frameBindingSet;
    target.drawConstantsSet = frame.drawConstantsSet;
    RecordMaterialPass(
        commandList,
        frame.recorder,
        *frame.geometryPipelines,
        target,
        frame.OpaqueDrawItems(),
        [&](nvrhi::ICommandList* tailList)
        {
            if (frame.groundPlane)
            {
                // After the opaque items, so their depth rejects the ground's hidden pixels; before
                // the decals, which may lie on it.
                nvrhi::GraphicsState state;
                state.pipeline = m_groundPipeline;
                state.framebuffer = target.framebuffer;
                state.viewport = target.viewport;
                state.bindings = {frame.frameBindingSet};
                tailList->setGraphicsState(state);
                tailList->draw(nvrhi::DrawArguments().setVertexCount(3));
            }
            if (frame.decalPipelines != nullptr)
            {
                RecordMaterialDrawItems(tailList, *frame.decalPipelines, target, frame.decalDrawItems);
            }
        });
}

void VulkanGeometryPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateFramebuffers(targets);
}

const nvrhi::FramebufferInfo& VulkanGeometryPass::GetFramebufferInfo() const
{
    return m_framebuffers.front()->getFramebufferInfo();
}

void VulkanGeometryPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    m_framebuffers.clear();
    for (uint32_t slot = 0; slot < targets.GetTransientCopyCount(); ++slot)
    {
        nvrhi::FramebufferDesc desc;
        for (uint32_t index = 0; index < kColorAttachmentCount; ++index)
        {
            desc.addColorAttachment(targets.GetTexture(kAttachments[index], slot));
        }
        desc.setDepthAttachment(targets.GetTexture(RenderTargetId::SceneDepth, slot));
        nvrhi::FramebufferHandle framebuffer = m_nvrhiDevice->createFramebuffer(desc);
        if (!framebuffer)
        {
            throw std::runtime_error("Failed to create a geometry pass framebuffer");
        }
        m_framebuffers.push_back(framebuffer);
    }
}
}
