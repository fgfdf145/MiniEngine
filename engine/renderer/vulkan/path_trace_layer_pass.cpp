#include "path_trace_layer_pass.h"

#include "compute_pass_util.h"
#include "material_draw.h"
#include "nvrhi_pass.h"

#include <array>
#include <stdexcept>

namespace me
{

VulkanPathTraceLayerPass::VulkanPathTraceLayerPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_nvrhiDevice(nvrhiDevice)
{
    const nvrhi::FormatSupport needed = nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::Blendable | nvrhi::FormatSupport::ShaderSample;
    m_supported = (m_nvrhiDevice->queryFormatSupport(kDepthFormat) & needed) == needed;
    m_surfaceFormats = {
        ToNvrhiFormat(targets.GetFormat(RenderTargetId::GBufferAlbedo)),
        ToNvrhiFormat(targets.GetFormat(RenderTargetId::GBufferNormal)),
        ToNvrhiFormat(targets.GetFormat(RenderTargetId::GBufferSurface)),
        ToNvrhiFormat(targets.GetFormat(RenderTargetId::GBufferVelocity))};
    m_sceneDepthFormat = ToNvrhiFormat(targets.GetFormat(RenderTargetId::SceneDepth));
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
}

VulkanPathTraceLayerPass::~VulkanPathTraceLayerPass() = default;

ScenePassId VulkanPathTraceLayerPass::Id() const
{
    return ScenePassId::PathTraceLayer;
}

RenderPassIo VulkanPathTraceLayerPass::Io() const
{
    // The scene's depth, which the depth pass tests against as its depth target (never writing it).
    // Its own images the tracker does not follow.
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneDepth};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanPathTraceLayerPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    if (!frame.pathTraceLayer || !m_ready || frame.pathTraceLayerDepthPipelines == nullptr || frame.pathTraceLayerSurfacePipelines == nullptr)
    {
        return;
    }
    nvrhi::ICommandList* commandList = frame.commandList;
    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneDepth, frame.imageIndex, frame.frameSlot);
    const NvrhiPassScope scope(commandList, {{targets.GetTexture(RenderTargetId::SceneDepth, slot), nvrhi::ResourceStates::DepthWrite}});
    RecordInitialTransition(commandList);
    const std::span<const VulkanDrawItem> items = frame.PathTraceLayerDrawItems();
    MaterialDrawTarget target;
    target.viewport = NativeViewportState(frame.extent);
    target.frameSet = frame.frameBindingSet;
    target.drawConstantsSet = frame.drawConstantsSet;

    // Every image is redrawn; the depth is cleared to 0, the far plane, which the path tracer reads as
    // no surface, whether or not anything is drawn.
    ClearTextureFloat(commandList, m_depth, nvrhi::Color(0.0f));
    for (const nvrhi::TextureHandle& image : m_surfaceImages)
    {
        ClearTextureFloat(commandList, image, nvrhi::Color(0.0f));
    }
    commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();
    target.framebuffer = m_depthFramebuffers.at(slot);
    RecordMaterialDrawItems(commandList, *frame.pathTraceLayerDepthPipelines, target, items);

    // The surface pass reads the depth found above.
    commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    for (const nvrhi::TextureHandle& image : m_surfaceImages)
    {
        commandList->setTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    }
    commandList->commitBarriers();
    target.framebuffer = m_surfaceFramebuffer;
    RecordMaterialDrawItems(commandList, *frame.pathTraceLayerSurfacePipelines, target, items);

    // Where the path tracer and set 0 read them.
    for (const nvrhi::TextureHandle& image : m_surfaceImages)
    {
        commandList->setTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
}

void VulkanPathTraceLayerPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // Remade at the new size by the next path traced frame's Prepare.
    (void)targets;
    DestroyImages();
}

bool VulkanPathTraceLayerPass::IsSupported() const
{
    return m_supported;
}

nvrhi::FramebufferInfo VulkanPathTraceLayerPass::GetDepthFramebufferInfo() const
{
    nvrhi::FramebufferInfo info;
    info.colorFormats.push_back(kDepthFormat);
    info.depthFormat = m_sceneDepthFormat;
    return info;
}

nvrhi::FramebufferInfo VulkanPathTraceLayerPass::GetSurfaceFramebufferInfo() const
{
    nvrhi::FramebufferInfo info;
    for (const nvrhi::Format format : m_surfaceFormats)
    {
        info.colorFormats.push_back(format);
    }
    return info;
}

bool VulkanPathTraceLayerPass::Prepare(const SceneRenderTargets& targets)
{
    if (m_ready || !m_supported)
    {
        return false;
    }
    try
    {
        const VkExtent2D extent = targets.GetExtent();
        m_depth = CreateImage(kDepthFormat, extent, "Path traced layer depth");
        static constexpr std::array<const char*, kSurfaceImageCount> kNames = {
            "Path traced layer albedo", "Path traced layer normal", "Path traced layer surface", "Path traced layer velocity"};
        for (uint32_t index = 0; index < kSurfaceImageCount; ++index)
        {
            m_surfaceImages[index] = CreateImage(m_surfaceFormats[index], extent, kNames[index]);
        }
        for (uint32_t copy = 0; copy < targets.GetTransientCopyCount(); ++copy)
        {
            m_depthFramebuffers.push_back(CreateNvrhiFramebuffer(m_nvrhiDevice, {m_depth}, targets.GetTexture(RenderTargetId::SceneDepth, copy)));
        }
        nvrhi::FramebufferDesc surface;
        for (const nvrhi::TextureHandle& image : m_surfaceImages)
        {
            surface.addColorAttachment(image);
        }
        m_surfaceFramebuffer = m_nvrhiDevice->createFramebuffer(surface);
        if (!m_surfaceFramebuffer)
        {
            throw std::runtime_error("Failed to create the path traced layer's surface framebuffer");
        }
    }
    catch (...)
    {
        DestroyImages();
        throw;
    }
    m_ready = true;
    m_initialized = false;
    return true;
}

bool VulkanPathTraceLayerPass::IsReady() const
{
    return m_ready;
}

void VulkanPathTraceLayerPass::RecordInitialTransition(nvrhi::ICommandList* commandList) const
{
    if (m_initialized || !m_ready)
    {
        return;
    }
    // From nothing to where set 0 and the path tracer read them.
    commandList->setTextureState(m_depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    for (const nvrhi::TextureHandle& image : m_surfaceImages)
    {
        commandList->setTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
    m_initialized = true;
}

TextureDescriptorBinding VulkanPathTraceLayerPass::GetDepthBinding() const
{
    return BindTexture(VK_NULL_HANDLE, m_depth, m_sampler);
}

nvrhi::TextureHandle VulkanPathTraceLayerPass::CreateImage(nvrhi::Format format, VkExtent2D extent, const char* name) const
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
}

void VulkanPathTraceLayerPass::DestroyImages()
{
    m_ready = false;
    m_depthFramebuffers.clear();
    m_surfaceFramebuffer = nullptr;
    m_depth = nullptr;
    m_surfaceImages = {};
}
}
