#include "tonemap_pass.h"

#include "gbuffer_inputs.h"
#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <array>
#include <filesystem>
#include <vector>

namespace me
{

namespace
{
// The tone mapping push constant block. Must match TonemapConstants in shaders/vulkan/tonemap.frag.
struct TonemapPushConstants
{
    float exposure = 1.0f;
    uint32_t gbufferView = 0;
};

static_assert(sizeof(TonemapPushConstants) == 8, "TonemapPushConstants must match the shader's block");
}

VulkanTonemapPass::VulkanTonemapPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout gbufferSetLayout,
    VkDescriptorSetLayout emptySetLayout)
    : m_device(device)
{
    // A throw out of a constructor skips the destructor, so everything created before the failure
    // would leak with it. DestroyHandles skips null handles, so unwinding whatever got created is
    // the same call the destructor makes.
    try
    {
        CreateDescriptorSetLayout();
        CreateSampler();
        CreateRenderPass(targets);
        CreatePipeline(pipelineCache, gbufferSetLayout, emptySetLayout);
        CreateDescriptorSets(targets);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTonemapPass::~VulkanTonemapPass()
{
    DestroyHandles();
}

ScenePassId VulkanTonemapPass::Id() const
{
    return ScenePassId::Tonemap;
}

RenderPassIo VulkanTonemapPass::Io() const
{
    // Binding set 2 requires every image in it to be in the read layout whenever this pass
    // records, whether or not the selected view samples it, so all five G-buffer inputs are
    // declared reads alongside the HDR target. In an order that never wrote them their contents
    // are undefined, and the renderer forces the view off.
    static constexpr std::array<RenderTargetId, 6> kReads = {
        RenderTargetId::SceneHdr,
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::SceneDepth};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneLdr};

    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanTonemapPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // No clear values: the attachment's loadOp is DONT_CARE because the full-screen triangle
    // covers every pixel and writes all four channels.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    // There is one framebuffer per LDR copy and one descriptor set per HDR copy, so both indices
    // come from the target they belong to rather than from a rule repeated here.
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::SceneLdr, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = frame.extent;
    renderPassInfo.clearValueCount = 0;
    renderPassInfo.pClearValues = nullptr;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    SetViewportAndScissor(commandBuffer, frame.extent);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const VkDescriptorSet descriptorSet = m_descriptorSets.at(
        targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot));
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        0,
        1,
        &descriptorSet,
        0,
        nullptr);

    // Set 1 is never bound: its layout is empty.
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout,
        2,
        1,
        &frame.gbufferDescriptorSet,
        0,
        nullptr);

    TonemapPushConstants constants{};
    constants.exposure = frame.exposure;
    constants.gbufferView = static_cast<uint32_t>(frame.gbufferView);
    vkCmdPushConstants(
        commandBuffer,
        m_pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(constants),
        &constants);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
}

void VulkanTonemapPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    // Both sides of the pass follow the new images: the framebuffers attach the new LDR views and
    // the descriptor sets sample the new HDR views. The render pass and pipeline depend only on
    // the LDR format, which a rebuild does not change.
    DestroyFramebuffers();
    CreateFramebuffers(targets);
    CreateDescriptorSets(targets);
}

void VulkanTonemapPass::CreateDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create tone mapping descriptor set layout");
}

void VulkanTonemapPass::CreateSampler()
{
    // The pass samples one texel per pixel at matching resolution, so linear filtering would only
    // blur, and with one mip level there is nothing for a mip mode or a LOD range to select.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.mipLodBias = 0.0f;

    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create tone mapping sampler");
}

void VulkanTonemapPass::CreateRenderPass(const SceneRenderTargets& targets)
{
    m_renderPass = CreateFullscreenRenderPass(m_device, targets.GetFormat(RenderTargetId::SceneLdr), "tone mapping");
}

void VulkanTonemapPass::CreatePipeline(
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout gbufferSetLayout,
    VkDescriptorSetLayout emptySetLayout)
{
    const std::array<VkDescriptorSetLayout, 3> setLayouts = {m_setLayout, emptySetLayout, gbufferSetLayout};

    // The exposure changes every frame the user drags the slider, and the view whenever they pick
    // one, so both are push constants rather than something that would force the descriptor sets
    // to be rewritten.
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(TonemapPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create tone mapping pipeline layout");

    m_pipeline = CreateFullscreenPipeline(
        m_device,
        pipelineCache,
        m_renderPass,
        m_pipelineLayout,
        "tonemap.frag.spv",
        "tone mapping");
}

void VulkanTonemapPass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    // One set per HDR copy, so these are indexed by frame slot. The count is fixed
    // (kMaxFramesInFlight), so the pool is sized once and reused.
    const uint32_t copyCount = targets.GetTransientCopyCount();

    if (m_descriptorPool == VK_NULL_HANDLE)
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = copyCount;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = copyCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create tone mapping descriptor pool");
    }
    else
    {
        // Called again from OnTargetsRebuilt because the HDR views changed. Resetting the pool
        // returns the previous sets to it instead of leaking them.
        m_descriptorSets.clear();
        CheckVulkan(vkResetDescriptorPool(m_device, m_descriptorPool, 0), "Failed to reset tone mapping descriptor pool");
    }

    const std::vector<VkDescriptorSetLayout> layouts(copyCount, m_setLayout);

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = copyCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.assign(copyCount, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate tone mapping descriptor sets");

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_sampler;
        imageInfo.imageView = targets.GetSampledView(RenderTargetId::SceneHdr, slot);
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[slot];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

void VulkanTonemapPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    // One framebuffer per LDR copy, so these are indexed by swapchain image.
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetLdrCopyCount();
    m_framebuffers.reserve(copyCount);

    for (uint32_t imageIndex = 0; imageIndex < copyCount; ++imageIndex)
    {
        const VkImageView attachment = targets.GetView(RenderTargetId::SceneLdr, imageIndex);

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        // Appended one at a time so that a failure part way through still leaves every handle
        // created so far reachable by DestroyFramebuffers.
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create tone mapping framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanTonemapPass::DestroyFramebuffers()
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

void VulkanTonemapPass::DestroyHandles()
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
    // Destroying the pool frees every set allocated from it.
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
