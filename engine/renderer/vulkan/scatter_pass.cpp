#include "scatter_pass.h"

#include "compute_pass_util.h"
#include "material_draw.h"
#include "nvrhi_pass.h"
#include "reverse_depth.h"

#include <array>
#include <stdexcept>

namespace me
{

VulkanScatterPass::VulkanScatterPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_nvrhiDevice(nvrhiDevice)
{
    // The viewer reads the pre-pass with NEAREST: each sample is one surface point's light.
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
    CreateImages(targets.GetExtent());
}

VulkanScatterPass::~VulkanScatterPass() = default;

ScenePassId VulkanScatterPass::Id() const
{
    return ScenePassId::Scatter;
}

RenderPassIo VulkanScatterPass::Io() const
{
    // Its own images only, which the tracker does not follow.
    return RenderPassIo{};
}

void VulkanScatterPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& /*targets*/,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    nvrhi::ICommandList* commandList = frame.commandList;
    commandList->clearState();
    if (!m_initialized)
    {
        // Freshly made images to their resting state, once: the frame set names them there from the
        // first frame on, whether or not anything scatters.
        commandList->setTextureState(m_light, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
        m_initialized = true;
    }
    if (frame.scatterDrawItems.empty() || frame.scatterPipelines == nullptr)
    {
        return;
    }
    // Both images are cleared and redrawn, then rest where this frame's forward pass samples them.
    // Alpha 0 is no draw: the gather skips it as another surface's.
    ClearTextureFloat(commandList, m_light, nvrhi::Color(0.0f));
    ClearDepth(commandList, m_depth, kReverseDepthFar);
    commandList->setTextureState(m_light, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();
    MaterialDrawTarget target;
    target.framebuffer = m_framebuffer;
    target.viewport = NativeViewportState(m_extent);
    target.frameSet = frame.frameBindingSet;
    target.drawConstantsSet = frame.drawConstantsSet;
    RecordMaterialDrawItems(commandList, *frame.scatterPipelines, target, frame.scatterDrawItems);
    commandList->setTextureState(m_light, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    commandList->clearState();
}

void VulkanScatterPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateImages(targets.GetExtent());
}

const nvrhi::FramebufferInfo& VulkanScatterPass::GetFramebufferInfo() const
{
    return m_framebuffer->getFramebufferInfo();
}

TextureDescriptorBinding VulkanScatterPass::GetLightBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_light, m_sampler);
}

TextureDescriptorBinding VulkanScatterPass::GetDepthBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_depth, m_sampler);
}

void VulkanScatterPass::CreateImages(VkExtent2D extent)
{
    m_extent = extent;
    const auto create = [&](nvrhi::Format format, const char* name)
    {
        nvrhi::TextureDesc desc;
        desc.width = extent.width;
        desc.height = extent.height;
        desc.format = format;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.debugName = name;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        nvrhi::TextureHandle texture = m_nvrhiDevice->createTexture(desc);
        if (!texture)
        {
            throw std::runtime_error(std::string("Failed to create the ") + name);
        }
        return texture;
    };
    m_framebuffer = nullptr;
    m_light = create(nvrhi::Format::RGBA16_FLOAT, "Scatter pre-pass light");
    m_depth = create(nvrhi::Format::D32, "Scatter pre-pass depth");
    m_framebuffer = CreateNvrhiFramebuffer(m_nvrhiDevice, {m_light}, m_depth);
    m_initialized = false;
}
}
