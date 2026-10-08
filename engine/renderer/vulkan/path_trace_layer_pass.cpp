#include "path_trace_layer_pass.h"

#include "compute_pass_util.h"
#include "material_draw.h"
#include "nvrhi_resources.h"

#include <array>

namespace me
{

namespace
{
// In: last frame's fragment and compute shaders sampled the images this pass redraws. Out: the
// surface pass's fragment shader samples the depth, the path tracer every image.
std::array<VkSubpassDependency, 2> ImageDependencies()
{
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return dependencies;
}

// A redrawn colour attachment that rests in SHADER_READ_ONLY_OPTIMAL.
VkAttachmentDescription RedrawnColor(VkFormat format)
{
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return attachment;
}
}

VulkanPathTraceLayerPass::VulkanPathTraceLayerPass(VkPhysicalDevice physicalDevice, VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, kDepthFormat, &properties);
    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    m_supported = (properties.optimalTilingFeatures & kNeeded) == kNeeded;
    m_surfaceFormats = {
        targets.GetFormat(RenderTargetId::GBufferAlbedo),
        targets.GetFormat(RenderTargetId::GBufferNormal),
        targets.GetFormat(RenderTargetId::GBufferSurface),
        targets.GetFormat(RenderTargetId::GBufferVelocity)};
    try
    {
        m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_NEAREST);
        CreateRenderPasses(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanPathTraceLayerPass::~VulkanPathTraceLayerPass()
{
    DestroyHandles();
}

ScenePassId VulkanPathTraceLayerPass::Id() const
{
    return ScenePassId::PathTraceLayer;
}

RenderPassIo VulkanPathTraceLayerPass::Io() const
{
    // The scene's depth, which the depth pass tests against as its depth attachment (never writing
    // it). Its own images the tracker does not follow.
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
    if (!frame.pathTraceLayer || !m_ready || frame.pathTraceLayerDepthPipelines == nullptr || frame.pathTraceLayerSurfacePipelines == nullptr)
    {
        return;
    }
    RecordInitialTransition(commandBuffer);
    const std::span<const VulkanDrawItem> items = frame.PathTraceLayerDrawItems();

    // Cleared to 0, the far plane, which the path tracer reads as no surface, whether or not anything
    // is drawn.
    VkClearValue depthClear{};
    depthClear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    depthInfo.renderPass = m_depthRenderPass;
    depthInfo.framebuffer = m_depthFramebuffers.at(targets.ResolveIndex(RenderTargetId::SceneDepth, frame.imageIndex, frame.frameSlot));
    depthInfo.renderArea.extent = frame.extent;
    depthInfo.clearValueCount = 1;
    depthInfo.pClearValues = &depthClear;
    vkCmdBeginRenderPass(commandBuffer, &depthInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    RecordMaterialDrawItems(commandBuffer, *frame.pathTraceLayerDepthPipelines, frame.frameDescriptorSet, items);
    vkCmdEndRenderPass(commandBuffer);

    std::array<VkClearValue, kSurfaceImageCount> surfaceClears{};
    VkRenderPassBeginInfo surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    surfaceInfo.renderPass = m_surfaceRenderPass;
    surfaceInfo.framebuffer = m_surfaceFramebuffer;
    surfaceInfo.renderArea.extent = frame.extent;
    surfaceInfo.clearValueCount = static_cast<uint32_t>(surfaceClears.size());
    surfaceInfo.pClearValues = surfaceClears.data();
    vkCmdBeginRenderPass(commandBuffer, &surfaceInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    RecordMaterialDrawItems(commandBuffer, *frame.pathTraceLayerSurfacePipelines, frame.frameDescriptorSet, items);
    vkCmdEndRenderPass(commandBuffer);
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

VkRenderPass VulkanPathTraceLayerPass::GetDepthRenderPass() const
{
    return m_depthRenderPass;
}

VkRenderPass VulkanPathTraceLayerPass::GetSurfaceRenderPass() const
{
    return m_surfaceRenderPass;
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
        CreateImage(kDepthFormat, extent, m_depth);
        for (uint32_t index = 0; index < kSurfaceImageCount; ++index)
        {
            CreateImage(m_surfaceFormats[index], extent, m_surfaceImages[index]);
        }
        for (uint32_t copy = 0; copy < targets.GetTransientCopyCount(); ++copy)
        {
            const std::array<VkImageView, 2> views = {m_depth.view, targets.GetView(RenderTargetId::SceneDepth, copy)};
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_depthRenderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(views.size());
            framebufferInfo.pAttachments = views.data();
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create the path traced layer's depth framebuffer");
            m_depthFramebuffers.push_back(framebuffer);
        }
        std::array<VkImageView, kSurfaceImageCount> views{};
        for (uint32_t index = 0; index < kSurfaceImageCount; ++index)
        {
            views[index] = m_surfaceImages[index].view;
        }
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_surfaceRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(views.size());
        framebufferInfo.pAttachments = views.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_surfaceFramebuffer), "Failed to create the path traced layer's surface framebuffer");
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

void VulkanPathTraceLayerPass::RecordInitialTransition(VkCommandBuffer commandBuffer) const
{
    if (m_initialized || !m_ready)
    {
        return;
    }
    std::array<VkImageMemoryBarrier, kSurfaceImageCount + 1> barriers{};
    for (uint32_t index = 0; index < barriers.size(); ++index)
    {
        VkImageMemoryBarrier& barrier = barriers[index];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = index == 0 ? m_depth.image : m_surfaceImages[index - 1].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());
    m_initialized = true;
}

TextureDescriptorBinding VulkanPathTraceLayerPass::GetDepthBinding() const
{
    return BindTexture(m_depth.view, m_depth.texture, m_sampler);
}

VkImageView VulkanPathTraceLayerPass::GetDepthView() const
{
    return m_depth.view;
}

VkImageView VulkanPathTraceLayerPass::GetAlbedoView() const
{
    return m_surfaceImages[kAlbedo].view;
}

VkImageView VulkanPathTraceLayerPass::GetNormalView() const
{
    return m_surfaceImages[kNormal].view;
}

VkImageView VulkanPathTraceLayerPass::GetSurfaceView() const
{
    return m_surfaceImages[kSurface].view;
}

VkImageView VulkanPathTraceLayerPass::GetVelocityView() const
{
    return m_surfaceImages[kVelocity].view;
}

void VulkanPathTraceLayerPass::CreateRenderPasses(const SceneRenderTargets& targets)
{
    const std::array<VkSubpassDependency, 2> dependencies = ImageDependencies();

    // Depth: the nearest surface's depth into the R32F image; the scene's depth tested as it stands,
    // in the layout the tracker put it in.
    VkAttachmentDescription sceneDepth{};
    sceneDepth.format = targets.GetFormat(RenderTargetId::SceneDepth);
    sceneDepth.samples = VK_SAMPLE_COUNT_1_BIT;
    sceneDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    sceneDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    sceneDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sceneDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    sceneDepth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    sceneDepth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const std::array<VkAttachmentDescription, 2> depthAttachments = {RedrawnColor(kDepthFormat), sceneDepth};
    const VkAttachmentReference depthColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference sceneDepthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription depthSubpass{};
    depthSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    depthSubpass.colorAttachmentCount = 1;
    depthSubpass.pColorAttachments = &depthColorRef;
    depthSubpass.pDepthStencilAttachment = &sceneDepthRef;
    VkRenderPassCreateInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    depthInfo.attachmentCount = static_cast<uint32_t>(depthAttachments.size());
    depthInfo.pAttachments = depthAttachments.data();
    depthInfo.subpassCount = 1;
    depthInfo.pSubpasses = &depthSubpass;
    depthInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    depthInfo.pDependencies = dependencies.data();
    CheckVulkan(vkCreateRenderPass(m_device, &depthInfo, nullptr, &m_depthRenderPass), "Failed to create the path traced layer's depth pass");

    // Surface: gbuffer.frag's eight outputs, four of them kept, no depth attachment.
    std::array<VkAttachmentDescription, kSurfaceImageCount> surfaceAttachments{};
    for (uint32_t index = 0; index < kSurfaceImageCount; ++index)
    {
        surfaceAttachments[index] = RedrawnColor(m_surfaceFormats[index]);
    }
    std::array<VkAttachmentReference, kSurfaceColorSlots> surfaceRefs{};
    surfaceRefs.fill(VkAttachmentReference{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED});
    surfaceRefs[0] = VkAttachmentReference{kAlbedo, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    surfaceRefs[1] = VkAttachmentReference{kNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    surfaceRefs[2] = VkAttachmentReference{kSurface, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    surfaceRefs[4] = VkAttachmentReference{kVelocity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription surfaceSubpass{};
    surfaceSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    surfaceSubpass.colorAttachmentCount = static_cast<uint32_t>(surfaceRefs.size());
    surfaceSubpass.pColorAttachments = surfaceRefs.data();
    VkRenderPassCreateInfo surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    surfaceInfo.attachmentCount = static_cast<uint32_t>(surfaceAttachments.size());
    surfaceInfo.pAttachments = surfaceAttachments.data();
    surfaceInfo.subpassCount = 1;
    surfaceInfo.pSubpasses = &surfaceSubpass;
    surfaceInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    surfaceInfo.pDependencies = dependencies.data();
    CheckVulkan(vkCreateRenderPass(m_device, &surfaceInfo, nullptr, &m_surfaceRenderPass), "Failed to create the path traced layer's surface pass");
}

void VulkanPathTraceLayerPass::CreateImage(VkFormat format, VkExtent2D extent, Image& image) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, image.image, "Failed to create a path traced layer image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create a path traced layer view");
}

void VulkanPathTraceLayerPass::DestroyImages()
{
    m_ready = false;
    for (VkFramebuffer framebuffer : m_depthFramebuffers)
    {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    m_depthFramebuffers.clear();
    if (m_surfaceFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_device, m_surfaceFramebuffer, nullptr);
        m_surfaceFramebuffer = VK_NULL_HANDLE;
    }
    std::array<Image*, kSurfaceImageCount + 1> images = {&m_depth, &m_surfaceImages[0], &m_surfaceImages[1], &m_surfaceImages[2], &m_surfaceImages[3]};
    for (Image* image : images)
    {
        if (image->view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image->view, nullptr);
        }
        // The image and its memory go with the texture, released with the rest below.
        *image = Image{};
    }
}

void VulkanPathTraceLayerPass::DestroyHandles()
{
    DestroyImages();
    for (VkRenderPass* renderPass : {&m_depthRenderPass, &m_surfaceRenderPass})
    {
        if (*renderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_device, *renderPass, nullptr);
            *renderPass = VK_NULL_HANDLE;
        }
    }
    m_sampler = nullptr;
}
}
