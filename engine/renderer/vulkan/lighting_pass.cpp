#include "lighting_pass.h"

#include "gbuffer_inputs.h"
#include "pipeline.h"

#include <array>

namespace me
{

VulkanLightingPass::VulkanLightingPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout emptySetLayout,
    VkDescriptorSetLayout gbufferSetLayout)
    : m_device(device)
{
    try
    {
        m_renderPass = CreateFullscreenRenderPass(m_device, targets.GetFormat(RenderTargetId::SceneHdr), "lighting");
        CreatePipeline(pipelineCache, frameSetLayout, emptySetLayout, gbufferSetLayout);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanLightingPass::~VulkanLightingPass()
{
    DestroyHandles();
}

ScenePassId VulkanLightingPass::Id() const
{
    return ScenePassId::Lighting;
}

RenderPassIo VulkanLightingPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneHdr};

    RenderPassIo io{};
    io.reads = VulkanGBufferDescriptors::kInputs;
    io.writes = kWrites;
    return io;
}

void VulkanLightingPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // No clear values: the attachment's loadOp is DONT_CARE because the triangle writes every
    // pixel, background included.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = frame.extent;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Set 0 is the same per-swapchain-image camera set the material passes bind; its layout
    // already includes the fragment stage. Set 2 is the per-frame-slot G-buffer set. Set 1 is
    // never bound: its layout is empty.
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        0,
        1,
        &frame.frameDescriptorSet,
        0,
        nullptr);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        2,
        1,
        &frame.gbufferDescriptorSet,
        0,
        nullptr);

    // The same helper the forward pass clears with, so the two orders' backgrounds cannot differ.
    const glm::vec4 backgroundRadiance(GetBackgroundRadiance(frame.exposure), 1.0f);
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(backgroundRadiance),
        &backgroundRadiance);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanLightingPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // The render pass and pipeline depend only on the HDR format, which a rebuild never changes.
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

void VulkanLightingPass::CreatePipeline(
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout emptySetLayout,
    VkDescriptorSetLayout gbufferSetLayout)
{
    const std::array<VkDescriptorSetLayout, 3> setLayouts = {frameSetLayout, emptySetLayout, gbufferSetLayout};

    // The background radiance follows the exposure every frame, so it is a push constant.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::vec4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create lighting pipeline layout");

    m_pipeline = CreateFullscreenPipeline(
        m_device,
        pipelineCache,
        m_renderPass,
        m_pipelineLayout,
        "deferred_lighting.frag.spv",
        "lighting");
}

void VulkanLightingPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkImageView attachment = targets.GetView(RenderTargetId::SceneHdr, slot);

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create lighting framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanLightingPass::DestroyFramebuffers()
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

void VulkanLightingPass::DestroyHandles()
{
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
