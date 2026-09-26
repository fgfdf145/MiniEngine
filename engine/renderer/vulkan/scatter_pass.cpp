#include "scatter_pass.h"

#include "compute_pass_util.h"
#include "material_draw.h"

#include <array>

namespace me
{

namespace
{
VkImageMemoryBarrier InitialBarrier(VkImage image, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return barrier;
}
}

VulkanScatterPass::VulkanScatterPass(VkPhysicalDevice physicalDevice, VkDevice device, const SceneRenderTargets& targets)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        // The viewer reads the pre-pass with NEAREST: each sample is one surface point's light.
        m_sampler = CreateClampSampler(m_device, VK_FILTER_NEAREST);
        CreateRenderPass();
        CreateImages(targets.GetExtent());
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanScatterPass::~VulkanScatterPass()
{
    DestroyHandles();
}

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
    RecordInitialTransition(commandBuffer);
    if (frame.scatterDrawItems.empty() || frame.scatterPipelines == nullptr)
    {
        return;
    }

    std::array<VkClearValue, 2> clearValues{};
    // Alpha 0 is no draw: the gather skips it as another surface's.
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.extent = m_extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, m_extent);
    RecordMaterialDrawItems(commandBuffer, *frame.scatterPipelines, frame.frameDescriptorSet, frame.scatterDrawItems);
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanScatterPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyImages();
    CreateImages(targets.GetExtent());
}

VkRenderPass VulkanScatterPass::GetRenderPass() const
{
    return m_renderPass;
}

TextureDescriptorBinding VulkanScatterPass::GetLightBinding() const
{
    return TextureDescriptorBinding{m_light.view, m_sampler};
}

TextureDescriptorBinding VulkanScatterPass::GetDepthBinding() const
{
    return TextureDescriptorBinding{m_depth.view, m_sampler};
}

void VulkanScatterPass::CreateRenderPass()
{
    // Both images are cleared and redrawn, so they come in UNDEFINED and leave in the layout set 0
    // samples them in.
    VkAttachmentDescription light{};
    light.format = kLightFormat;
    light.samples = VK_SAMPLE_COUNT_1_BIT;
    light.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    light.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    light.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    light.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    light.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    light.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depth = light;
    depth.format = kDepthFormat;

    const VkAttachmentReference lightRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &lightRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // In: the previous frame's fragment shaders sampled both images, and this frame overwrites them.
    // Out: this frame's forward pass samples what was drawn.
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {light, depth};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();
    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create the scatter pre-pass");
}

void VulkanScatterPass::CreateImages(VkExtent2D extent)
{
    m_extent = extent;
    CreateImage(kLightFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, extent, m_light);
    CreateImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, extent, m_depth);

    const std::array<VkImageView, 2> views = {m_light.view, m_depth.view};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(views.size());
    framebufferInfo.pAttachments = views.data();
    framebufferInfo.width = extent.width;
    framebufferInfo.height = extent.height;
    framebufferInfo.layers = 1;
    CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer), "Failed to create the scatter pre-pass framebuffer");
    m_initialized = false;
}

void VulkanScatterPass::CreateImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkExtent2D extent, Image& image) const
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
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &image.image), "Failed to create a scatter pre-pass image");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, image.image, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(m_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &image.memory), "Failed to allocate a scatter pre-pass image");
    CheckVulkan(vkBindImageMemory(m_device, image.image, image.memory, 0), "Failed to bind a scatter pre-pass image");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create a scatter pre-pass view");
}

void VulkanScatterPass::RecordInitialTransition(VkCommandBuffer commandBuffer) const
{
    if (m_initialized)
    {
        return;
    }
    const std::array<VkImageMemoryBarrier, 2> barriers = {
        InitialBarrier(m_light.image, VK_IMAGE_ASPECT_COLOR_BIT),
        InitialBarrier(m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT)};
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());
    m_initialized = true;
}

void VulkanScatterPass::DestroyImages()
{
    if (m_framebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    for (Image* image : {&m_light, &m_depth})
    {
        if (image->view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image->view, nullptr);
        }
        if (image->image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, image->image, nullptr);
        }
        if (image->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, image->memory, nullptr);
        }
        *image = Image{};
    }
}

void VulkanScatterPass::DestroyHandles()
{
    DestroyImages();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}
}
