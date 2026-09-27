#include "geometry_pass.h"

#include "material_draw.h"
#include "pipeline.h"

namespace me
{

VulkanGeometryPass::VulkanGeometryPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout)
    : m_device(device)
{
    // A throw out of a constructor skips the destructor, so unwind whatever got created with the
    // same call the destructor makes.
    try
    {
        CreateRenderPass(targets);
        CreateFramebuffers(targets);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &frameSetLayout;
        CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_groundPipelineLayout), "Failed to create ground plane pipeline layout");
        FullscreenPipelineOptions groundOptions{};
        groundOptions.depthTestAndWrite = true;
        groundOptions.colorAttachmentCount = kColorAttachmentCount;
        m_groundPipeline = CreateFullscreenPipeline(
            m_device, pipelineCache, m_renderPass, m_groundPipelineLayout, "ground.frag.spv", "ground plane", groundOptions);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanGeometryPass::~VulkanGeometryPass()
{
    DestroyHandles();
}

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
    // The lighting pass identifies background pixels by depth == 1.0, never by these color clears,
    // so zero serves; it also makes unwritten G-buffer pixels read as black in the debug views.
    std::array<VkClearValue, kAttachments.size()> clearValues{};
    clearValues[kColorAttachmentCount].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::GBufferAlbedo, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = frame.extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    RecordMaterialDrawItems(commandBuffer, *frame.geometryPipelines, frame.frameDescriptorSet, frame.OpaqueDrawItems());
    if (frame.groundPlane)
    {
        // After the opaque items, so their depth rejects the ground's hidden pixels; before the
        // decals, which may lie on it.
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_groundPipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_groundPipelineLayout, 0, 1, &frame.frameDescriptorSet, 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }
    if (frame.decalPipelines != nullptr)
    {
        RecordMaterialDrawItems(commandBuffer, *frame.decalPipelines, frame.frameDescriptorSet, frame.decalDrawItems);
    }
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanGeometryPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

VkRenderPass VulkanGeometryPass::GetRenderPass() const
{
    return m_renderPass;
}

void VulkanGeometryPass::CreateRenderPass(const SceneRenderTargets& targets)
{
    std::array<VkAttachmentDescription, kAttachments.size()> attachments{};
    std::array<VkAttachmentReference, kColorAttachmentCount> colorReferences{};

    for (uint32_t index = 0; index < static_cast<uint32_t>(kAttachments.size()); ++index)
    {
        const RenderTargetId target = kAttachments[index];
        // initialLayout == finalLayout == the write layout: this pass performs no implicit
        // transition, which is what keeps RenderTargetLayoutTracker the single authority.
        const VkImageLayout layout = GetWriteLayout(target);

        VkAttachmentDescription& attachment = attachments[index];
        attachment.format = targets.GetFormat(target);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = layout;
        attachment.finalLayout = layout;

        if (index < kColorAttachmentCount)
        {
            colorReferences[index].attachment = index;
            colorReferences[index].layout = layout;
        }
    }

    VkAttachmentReference depthReference{};
    depthReference.attachment = kColorAttachmentCount;
    depthReference.layout = GetWriteLayout(RenderTargetId::SceneDepth);

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = kColorAttachmentCount;
    subpass.pColorAttachments = colorReferences.data();
    subpass.pDepthStencilAttachment = &depthReference;

    // No subpass dependencies, as in every pass in the frame: the tracker's explicit barriers
    // carry the ordering.
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create geometry pass render pass");
}

void VulkanGeometryPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        std::array<VkImageView, kAttachments.size()> views{};
        for (size_t index = 0; index < kAttachments.size(); ++index)
        {
            views[index] = targets.GetView(kAttachments[index], slot);
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(views.size());
        framebufferInfo.pAttachments = views.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        // Appended one at a time so a failure part way through leaves every handle created so far
        // reachable by DestroyFramebuffers.
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create geometry pass framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanGeometryPass::DestroyFramebuffers()
{
    for (VkFramebuffer framebuffer : m_framebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
    }
    m_framebuffers.clear();
}

void VulkanGeometryPass::DestroyHandles()
{
    if (m_groundPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_groundPipeline, nullptr);
        m_groundPipeline = VK_NULL_HANDLE;
    }
    if (m_groundPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_groundPipelineLayout, nullptr);
        m_groundPipelineLayout = VK_NULL_HANDLE;
    }
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
