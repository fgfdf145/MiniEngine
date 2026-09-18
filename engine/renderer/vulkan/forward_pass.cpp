#include "forward_pass.h"

#include "command.h"

#include <engine/renderer/camera.h>
#include <engine/renderer/material.h>

#include <array>

namespace me
{

VulkanForwardPass::VulkanForwardPass(VkDevice device, const SceneRenderTargets& targets)
    : m_device(device)
{
    // A throw out of a constructor skips the destructor, so everything created before the failure
    // would leak with it. DestroyHandles skips null handles, so unwinding whatever got created is
    // the same call the destructor makes.
    try
    {
        CreateRenderPass(targets);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanForwardPass::~VulkanForwardPass()
{
    DestroyHandles();
}

ScenePassId VulkanForwardPass::Id() const
{
    return ScenePassId::Forward;
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
    std::array<VkClearValue, 2> clearValues{};
    // The clear lands in the HDR target and is exposed and tone mapped with everything else.
    // Dividing the exposed background by the exposure keeps it fixed while the exposure moves,
    // since it stands for no physical light.
    const glm::vec3 background = kViewportBackgroundExposed / frame.exposure;
    clearValues[0].color.float32[0] = background.r;
    clearValues[0].color.float32[1] = background.g;
    clearValues[0].color.float32[2] = background.b;
    clearValues[0].color.float32[3] = 1.0f;
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = frame.extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport and scissor are dynamic state on every material pipeline, so they are set once
    // per pass instead of being baked into the pipelines (see VulkanPipelineSet).
    VkViewport viewport{};
    viewport.width = static_cast<float>(frame.extent.width);
    viewport.height = static_cast<float>(frame.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = frame.extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const VkPipelineLayout pipelineLayout = frame.pipelines->GetLayout();
    VkPipeline boundPipeline = VK_NULL_HANDLE;

    // Set 0 (the camera uniform buffer) is the same for every draw in this pass, so it is bound
    // once here rather than per draw item. Set 1 (the material samplers) still varies per draw
    // item and is bound inside the loop below.
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout,
        0,
        1,
        &frame.frameDescriptorSet,
        0,
        nullptr);

    for (const VulkanDrawItem& drawItem : frame.drawItems)
    {
        const VkPipeline requiredPipeline = frame.pipelines->Get(drawItem.pipelineKey);
        if (requiredPipeline != boundPipeline)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, requiredPipeline);
            boundPipeline = requiredPipeline;
        }

        const VkBuffer vertexBuffers[] = {drawItem.vertexBuffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, drawItem.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            1,
            1,
            &drawItem.descriptorSet,
            0,
            nullptr);
        vkCmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(ObjectPushConstants),
            &drawItem.drawConstants);
        vkCmdDrawIndexed(commandBuffer, drawItem.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

void VulkanForwardPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

VkRenderPass VulkanForwardPass::GetRenderPass() const
{
    return m_renderPass;
}

void VulkanForwardPass::CreateRenderPass(const SceneRenderTargets& targets)
{
    // Both attachments keep the layout the tracker's barriers already put them in: this pass
    // performs no implicit transition, which is what keeps RenderTargetLayoutTracker the single
    // authority on layouts.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = targets.GetFormat(RenderTargetId::SceneHdr);
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = targets.GetFormat(RenderTargetId::SceneDepth);
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Stored, not discarded: SceneDepth is created sampleable so that a later pass in the frame
    // (deferred lighting, and anything else that reconstructs position from depth) can read it,
    // and DONT_CARE would leave that pass reading undefined contents. Stencil is never written.
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    const std::array<VkAttachmentDescription, 2> attachments = {
        colorAttachment,
        depthAttachment};

    // No subpass dependencies: the explicit barriers the layout tracker produces carry the
    // ordering that VulkanSceneViewport's two VK_SUBPASS_EXTERNAL dependencies used to, and
    // leaving them in would duplicate it against layouts that no longer change.
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 0;
    renderPassInfo.pDependencies = nullptr;

    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create forward pass render pass");
}

void VulkanForwardPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const std::array<VkImageView, 2> attachments = {
            targets.GetView(RenderTargetId::SceneHdr, slot),
            targets.GetView(RenderTargetId::SceneDepth, slot)};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        // Appended one at a time so that a failure part way through still leaves every handle
        // created so far reachable by DestroyFramebuffers.
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create forward pass framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanForwardPass::DestroyFramebuffers()
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

void VulkanForwardPass::DestroyHandles()
{
    DestroyFramebuffers();
    // Nulled after destruction so the handle is never left dangling: the constructor's unwind path
    // runs this and then throws, and GetRenderPass must not hand out a destroyed render pass.
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
