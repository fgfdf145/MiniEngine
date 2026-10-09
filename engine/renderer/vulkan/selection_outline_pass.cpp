#include "selection_outline_pass.h"

#include "buffer.h"
#include "pipeline.h"
#include "reverse_depth.h"
#include "sampler_settings.h"

#include <engine/core/paths/engine_paths.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace me
{

namespace
{
// The outline pass's push constant block. Must match OutlineConstants in
// shaders/vulkan/selection_outline.frag.
struct SelectionOutlinePushConstants
{
    // Linear rgb; a is unused.
    glm::vec4 color{0.0f};
    float width = 2.0f;
    // The line's opacity where something nearer hides the entity.
    float occludedOpacity = 0.35f;
    // How far, in pixels, the shader looks for the silhouette: ceil(width).
    int32_t radius = 2;
    // 0 without a selection: the shader writes transparent pixels and samples nothing.
    uint32_t enabled = 0;
};

static_assert(sizeof(SelectionOutlinePushConstants) == 32, "SelectionOutlinePushConstants must match the shader's block");

// Blender's default theme: the active object's outline (#FFAA40), and how much of it shows through
// what hides the object in solid mode (overlay_outline.cc's alpha_occlu).
constexpr std::array<uint8_t, 3> kActiveObjectSrgb = {0xFF, 0xAA, 0x40};
constexpr float kOccludedOpacity = 0.35f;

float SrgbToLinear(uint8_t value)
{
    const float encoded = static_cast<float>(value) / 255.0f;
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}
}

VulkanSelectionMaskPass::VulkanSelectionMaskPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout materialSetLayout)
    : m_device(device)
{
    // A throw out of a constructor skips the destructor; DestroyHandles skips null handles, so the
    // unwind path and the destructor share it.
    try
    {
        CreateRenderPass(targets);
        CreatePipelines(pipelineCache, materialSetLayout);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanSelectionMaskPass::~VulkanSelectionMaskPass()
{
    DestroyHandles();
}

ScenePassId VulkanSelectionMaskPass::Id() const
{
    return ScenePassId::SelectionMask;
}

RenderPassIo VulkanSelectionMaskPass::Io() const
{
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SelectionDepth};

    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanSelectionMaskPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Cleared every frame, selection or none: the outline pass reads it either way.
    VkClearValue clearValue{};
    clearValue.depthStencil = {kReverseDepthFar, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::SelectionDepth, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.extent = frame.outputExtent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    SetViewportAndScissor(commandBuffer, frame.outputExtent);

    VkPipeline boundPipeline = VK_NULL_HANDLE;
    for (const ShadowDrawItem& item : frame.selectionDrawItems)
    {
        const VkPipeline requiredPipeline = item.alphaMask ? m_maskPipeline : m_opaquePipeline;
        if (requiredPipeline != boundPipeline)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, requiredPipeline);
            boundPipeline = requiredPipeline;
        }
        if (item.alphaMask)
        {
            vkCmdBindDescriptorSets(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout,
                0,
                1,
                &item.materialDescriptorSet,
                0,
                nullptr);
        }

        ShadowPushConstants constants{};
        constants.lightModelViewProjection = frame.selectionViewProjection * item.model;
        std::memcpy(constants.baseColorFactor, item.material.baseColorFactor, sizeof(constants.baseColorFactor));
        std::memcpy(constants.nodeGraphFactors, item.material.nodeGraphFactors, sizeof(constants.nodeGraphFactors));
        constants.alphaCutoffAndPadding[0] = item.material.alphaCutoff;
        std::memcpy(constants.baseColorTransform, item.baseColorTransform, sizeof(constants.baseColorTransform));
        vkCmdPushConstants(
            commandBuffer,
            m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(ShadowPushConstants),
            &constants);

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, item.alphaMask ? &item.vertexBuffer : &item.positionBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, item.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(commandBuffer, item.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);
}

void VulkanSelectionMaskPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

void VulkanSelectionMaskPass::CreateRenderPass(const SceneRenderTargets& targets)
{
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = targets.GetFormat(RenderTargetId::SelectionDepth);
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // initialLayout == finalLayout: RenderTargetLayoutTracker owns the transitions.
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 0;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthReference;

    // No subpass dependencies: the tracker's explicit barriers carry the ordering.
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create selection mask render pass");
}

void VulkanSelectionMaskPass::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout materialSetLayout)
{
    // The shadow casters' shaders: the light's view-projection is the camera's here.
    const std::filesystem::path shaderDir = EnginePaths::ShaderRoot();
    const VulkanShaderModule vertexShader(m_device, shaderDir / "shadow.vert.spv");
    const VulkanShaderModule fragmentShader(m_device, shaderDir / "shadow.frag.spv");
    const VulkanShaderModule depthVertexShader(m_device, shaderDir / "shadow_depth.vert.spv");

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &materialSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout), "Failed to create selection mask pipeline layout");

    const VkVertexInputBindingDescription bindingDescription = GetVertexBindingDescription();
    const auto allAttributes = GetVertexAttributeDescriptions();
    // Position and both UV sets: the alpha test may sample the base colour through either.
    const std::array<VkVertexInputAttributeDescription, 3> attributes = {allAttributes[0], allAttributes[2], allAttributes[5]};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDescription;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    // Everything else reads positions alone, from their own tightly packed stream.
    const VkVertexInputBindingDescription positionBinding = GetPositionBindingDescription();
    const VkVertexInputAttributeDescription positionAttribute = GetPositionAttributeDescription();
    VkPipelineVertexInputStateCreateInfo depthVertexInput{};
    depthVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    depthVertexInput.vertexBindingDescriptionCount = 1;
    depthVertexInput.pVertexBindingDescriptions = &positionBinding;
    depthVertexInput.vertexAttributeDescriptionCount = 1;
    depthVertexInput.pVertexAttributeDescriptions = &positionAttribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // No culling: the silhouette is the same from either side of a single-sided surface.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // The entity's nearest surface, reverse-Z like the scene's, which the outline pass compares it
    // with.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = kReverseDepthNearer;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader.GetHandle();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader.GetHandle();
    stages[1].pName = "main";

    VkPipelineShaderStageCreateInfo depthStage{};
    depthStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    depthStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    depthStage.module = depthVertexShader.GetHandle();
    depthStage.pName = "main";

    std::array<VkGraphicsPipelineCreateInfo, 2> pipelineInfos{};
    for (VkGraphicsPipelineCreateInfo& pipelineInfo : pipelineInfos)
    {
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
    }
    // Opaque: the position stream and no fragment stage.
    pipelineInfos[0].stageCount = 1;
    pipelineInfos[0].pStages = &depthStage;
    pipelineInfos[0].pVertexInputState = &depthVertexInput;
    // Mask: the full vertex and the alpha test.
    pipelineInfos[1].stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfos[1].pStages = stages.data();
    pipelineInfos[1].pVertexInputState = &vertexInput;

    std::array<VkPipeline, 2> pipelines{};
    CheckVulkan(
        vkCreateGraphicsPipelines(
            m_device,
            pipelineCache,
            static_cast<uint32_t>(pipelineInfos.size()),
            pipelineInfos.data(),
            nullptr,
            pipelines.data()),
        "Failed to create selection mask pipelines");
    m_opaquePipeline = pipelines[0];
    m_maskPipeline = pipelines[1];
}

void VulkanSelectionMaskPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetOutputExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const VkImageView attachment = targets.GetView(RenderTargetId::SelectionDepth, slot);

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create selection mask framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanSelectionMaskPass::DestroyFramebuffers()
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

void VulkanSelectionMaskPass::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_opaquePipeline, &m_maskPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
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

VulkanSelectionOutlinePass::VulkanSelectionOutlinePass(
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets)
    : m_device(device)
{
    try
    {
        CreateDescriptorSetLayout();
        CreateSampler(nvrhiDevice);
        CreatePipeline(pipelineCache, targets);
        CreateDescriptorSets(targets);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanSelectionOutlinePass::~VulkanSelectionOutlinePass()
{
    DestroyHandles();
}

ScenePassId VulkanSelectionOutlinePass::Id() const
{
    return ScenePassId::SelectionOutline;
}

RenderPassIo VulkanSelectionOutlinePass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {RenderTargetId::SelectionDepth, RenderTargetId::SceneDepth};
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SelectionOutline};

    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanSelectionOutlinePass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // No clear: the full-screen triangle writes every pixel, transparent where there is no line.
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(
        targets.ResolveIndex(RenderTargetId::SelectionOutline, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.extent = frame.outputExtent;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    SetViewportAndScissor(commandBuffer, frame.outputExtent);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const VkDescriptorSet descriptorSet = m_descriptorSets.at(
        targets.ResolveIndex(RenderTargetId::SelectionDepth, frame.imageIndex, frame.frameSlot));
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

    SelectionOutlinePushConstants constants{};
    constants.color = glm::vec4(
        SrgbToLinear(kActiveObjectSrgb[0]),
        SrgbToLinear(kActiveObjectSrgb[1]),
        SrgbToLinear(kActiveObjectSrgb[2]),
        1.0f);
    constants.width = std::max(frame.selectionOutlineWidth, 1.0f);
    constants.occludedOpacity = kOccludedOpacity;
    constants.radius = static_cast<int32_t>(std::ceil(constants.width));
    constants.enabled = frame.selectionDrawItems.empty() ? 0u : 1u;
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanSelectionOutlinePass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
    CreateDescriptorSets(targets);
}

void VulkanSelectionOutlinePass::CreateDescriptorSetLayout()
{
    // Binding 0 the selected entity's depth, binding 1 the scene's.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index)
    {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create selection outline descriptor set layout");
}

void VulkanSelectionOutlinePass::CreateSampler(nvrhi::IDevice* nvrhiDevice)
{
    // The shader fetches texels by index; the sampler is only what a combined image sampler needs.
    m_sampler = CreateNvrhiSampler(nvrhiDevice, BuildClampSamplerDesc(false), "Failed to create selection outline sampler");
}

void VulkanSelectionOutlinePass::CreatePipeline(VkPipelineCache pipelineCache, const SceneRenderTargets& targets)
{
    m_renderPass = CreateFullscreenRenderPass(m_device, targets.GetFormat(RenderTargetId::SelectionOutline), "selection outline");

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(SelectionOutlinePushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create selection outline pipeline layout");

    m_pipeline = CreateFullscreenPipeline(
        m_device,
        pipelineCache,
        m_renderPass,
        m_pipelineLayout,
        "selection_outline.frag.spv",
        "selection outline");
}

void VulkanSelectionOutlinePass::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();

    if (m_descriptorPool == VK_NULL_HANDLE)
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = copyCount * 2;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = copyCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create selection outline descriptor pool");
    }
    else
    {
        // Called again from OnTargetsRebuilt: resetting the pool returns the previous sets to it.
        m_descriptorSets.clear();
        CheckVulkan(vkResetDescriptorPool(m_device, m_descriptorPool, 0), "Failed to reset selection outline descriptor pool");
    }

    const std::vector<VkDescriptorSetLayout> layouts(copyCount, m_setLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = copyCount;
    allocateInfo.pSetLayouts = layouts.data();
    m_descriptorSets.assign(copyCount, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate selection outline descriptor sets");

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        std::array<VkDescriptorImageInfo, 2> imageInfos{};
        imageInfos[0].sampler = NativeSampler(m_sampler);
        imageInfos[0].imageView = targets.GetSampledView(RenderTargetId::SelectionDepth, slot);
        imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[1].sampler = NativeSampler(m_sampler);
        imageInfos[1].imageView = targets.GetSampledView(RenderTargetId::SceneDepth, slot);
        imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_descriptorSets[slot];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = &imageInfos[binding];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanSelectionOutlinePass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetOutputExtent();
    const uint32_t copyCount = targets.GetLdrCopyCount();
    m_framebuffers.reserve(copyCount);
    for (uint32_t imageIndex = 0; imageIndex < copyCount; ++imageIndex)
    {
        const VkImageView attachment = targets.GetView(RenderTargetId::SelectionOutline, imageIndex);

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create selection outline framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanSelectionOutlinePass::DestroyFramebuffers()
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

void VulkanSelectionOutlinePass::DestroyHandles()
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
    m_sampler = nullptr;
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
