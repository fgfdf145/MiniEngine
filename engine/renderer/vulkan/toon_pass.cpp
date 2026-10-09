#include "toon_pass.h"

#include "buffer.h"
#include "compute_pass_util.h"
#include "nvrhi_resources.h"
#include "pipeline.h"
#include "reverse_depth.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>

#include <cstring>
#include <filesystem>

namespace me
{

namespace
{
// The linear depth the prepass clears to: farther than any character, so where none is, the rim light
// sees background.
constexpr float kToonFarDepth = 1.0e6f;

// Which surface pipeline a toon draw takes: transparent ones blend (and leave the eye mask alone in
// the prepass), double-sided ones cull nothing.
size_t ToonPipelineIndex(const VulkanDrawItem& item)
{
    const bool transparent = item.toon->Has(kToonFeatureTransparent);
    return (transparent ? 2u : 0u) + (item.pipelineKey.doubleSided ? 1u : 0u);
}
constexpr size_t kOutlinePipeline = 4;

// How one toon pipeline differs from the next.
struct ToonPipelineDescription
{
    const VulkanShaderModule* vertexShader = nullptr;
    const VulkanShaderModule* fragmentShader = nullptr;
    bool outline = false;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkCompareOp depthCompare = kReverseDepthNearerOrEqual;
    // Pulls the surface a hair toward the camera, so that a toon draw lands on the depth the
    // geometry pass wrote for it from triangle.vert.
    bool depthBias = false;
    bool blend = false;
    // Per colour attachment: write it, or leave it as it is.
    std::array<bool, 2> writeAttachment = {true, true};
    uint32_t colorAttachmentCount = 2;
    // The prepass's targets: one channel each.
    bool singleChannel = false;
};

VkPipeline CreateToonPipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkRenderPass renderPass,
    VkPipelineLayout layout,
    const ToonPipelineDescription& description,
    const char* label)
{
    const std::array<VkVertexInputBindingDescription, 2> bindingDescriptions = {
        GetVertexBindingDescription(), GetPreviousPositionBindingDescription()};
    const auto attributeDescriptions = GetToonVertexAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
    vertexInput.pVertexBindingDescriptions = bindingDescriptions.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

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

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = description.cullMode;
    // As the material pipelines (VulkanPipelineSet).
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    // Reverse-Z: a positive bias moves toward the camera.
    rasterizer.depthBiasEnable = description.depthBias ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = description.depthBias ? 4.0f : 0.0f;
    rasterizer.depthBiasSlopeFactor = description.depthBias ? 1.0f : 0.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = description.depthCompare;

    std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments{};
    for (uint32_t index = 0; index < description.colorAttachmentCount; ++index)
    {
        VkPipelineColorBlendAttachmentState& attachment = blendAttachments[index];
        // The toon pass's two targets take rgb alike (the HDR target's alpha keeps its clear value, as
        // the forward pass leaves it; the velocity's b is the coat normal no toon surface has): every
        // attachment's state the same, which a device without independent blending requires. The
        // prepass writes its single-channel targets' r.
        attachment.colorWriteMask = description.writeAttachment[index]
                                        ? (description.singleChannel ? VK_COLOR_COMPONENT_R_BIT
                                                                     : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
                                        : 0u;
        if (description.blend)
        {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = description.colorAttachmentCount;
    colorBlending.pAttachments = blendAttachments.data();

    // Constant 0 of both stages: the outline (toon.vert pushes the hull out, toon.frag colours it).
    const VkBool32 outline = description.outline ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entry{0, 0, sizeof(VkBool32)};
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = 1;
    specialization.pMapEntries = &entry;
    specialization.dataSize = sizeof(outline);
    specialization.pData = &outline;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = description.vertexShader->GetHandle();
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = &specialization;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = description.fragmentShader->GetHandle();
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specialization;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    CheckVulkan(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline), label);
    return pipeline;
}

VkPipelineLayout CreateToonPipelineLayout(VkDevice device, std::span<const VkDescriptorSetLayout> setLayouts, const char* label)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(ToonPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CheckVulkan(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), label);
    return layout;
}

// Binds what a toon draw needs and draws it: its geometry, its material set and its constants.
void RecordToonDraw(
    VkCommandBuffer commandBuffer,
    VkPipelineLayout layout,
    const VulkanDrawItem& item,
    uint32_t toonIndex,
    float exposureScale)
{
    const std::array<VkBuffer, 2> buffers = {item.vertexBuffer, item.previousPositionBuffer};
    const std::array<VkDeviceSize, 2> offsets = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, buffers.data(), offsets.data());
    vkCmdBindIndexBuffer(commandBuffer, item.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &item.descriptorSet, 0, nullptr);
    ToonPushConstants constants{};
    constants.model = item.drawConstants.model;
    constants.toonIndex = toonIndex;
    constants.exposureScale = exposureScale;
    vkCmdPushConstants(
        commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDrawIndexed(commandBuffer, item.indexCount, 1, 0, 0, item.motionSlot);
}

void DestroyFramebufferList(VkDevice device, std::vector<VkFramebuffer>& framebuffers)
{
    for (VkFramebuffer framebuffer : framebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    framebuffers.clear();
}

template <size_t Count>
void DestroyPipelines(VkDevice device, std::array<VkPipeline, Count>& pipelines)
{
    for (VkPipeline& pipeline : pipelines)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
}
}

// ---------------------------------------------------------------------------------------------
// VulkanToonMaterials

VulkanToonMaterials::VulkanToonMaterials(VkPhysicalDevice physicalDevice, VkDevice device, nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount)
    : m_device(device)
{
    try
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create toon material set layout");

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = frameSlotCount;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = frameSlotCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool), "Failed to create toon material descriptor pool");

        const VkDeviceSize bytes = sizeof(GpuToonMaterial) * kMaxDraws;
        for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer buffer = VK_NULL_HANDLE;
            void* mapped = nullptr;
            m_handles.push_back(CreateNvrhiBuffer(
                nvrhiDevice,
                bufferInfo,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                buffer,
                "Failed to create toon material buffer",
                &mapped));
            m_buffers.push_back(buffer);
            m_mapped.push_back(mapped);
        }

        const std::vector<VkDescriptorSetLayout> layouts(frameSlotCount, m_setLayout);
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = m_pool;
        allocateInfo.descriptorSetCount = frameSlotCount;
        allocateInfo.pSetLayouts = layouts.data();
        m_sets.assign(frameSlotCount, VK_NULL_HANDLE);
        CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_sets.data()), "Failed to allocate toon material sets");
        for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
        {
            VkDescriptorBufferInfo bufferInfo{m_buffers[slot], 0, bytes};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_sets[slot];
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanToonMaterials::~VulkanToonMaterials()
{
    DestroyHandles();
}

uint32_t VulkanToonMaterials::Write(uint32_t frameSlot, std::span<const VulkanDrawItem> toonDrawItems, std::span<const glm::mat4> headPoses)
{
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(toonDrawItems.size(), kMaxDraws));
    auto* materials = static_cast<GpuToonMaterial*>(m_mapped.at(frameSlot));
    for (uint32_t index = 0; index < count; ++index)
    {
        GpuToonMaterial material = toonDrawItems[index].toon->gpu;
        if (index < headPoses.size())
        {
            const glm::mat4& pose = headPoses[index];
            const glm::vec3 position = glm::vec3(pose * glm::vec4(material.headPosition[0], material.headPosition[1], material.headPosition[2], 1.0f));
            const glm::vec3 forward = glm::normalize(glm::mat3(pose) * glm::vec3(material.headForward[0], material.headForward[1], material.headForward[2]));
            const glm::vec3 up = glm::normalize(glm::mat3(pose) * glm::vec3(material.headUp[0], material.headUp[1], material.headUp[2]));
            std::copy_n(&position.x, 3, material.headPosition);
            std::copy_n(&forward.x, 3, material.headForward);
            std::copy_n(&up.x, 3, material.headUp);
        }
        std::memcpy(&materials[index], &material, sizeof(GpuToonMaterial));
    }
    return count;
}

VkDescriptorSetLayout VulkanToonMaterials::GetSetLayout() const
{
    return m_setLayout;
}

VkDescriptorSet VulkanToonMaterials::GetSet(uint32_t frameSlot) const
{
    return m_sets.at(frameSlot);
}

void VulkanToonMaterials::DestroyHandles()
{
    // The buffers and their memory (unmapped as it is freed) go with the handles.
    m_mapped.clear();
    m_buffers.clear();
    m_handles.clear();
    if (m_pool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    m_sets.clear();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------------------------
// VulkanToonPrepass

VulkanToonPrepass::VulkanToonPrepass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout,
    const VulkanToonMaterials& materials)
    : m_device(device),
      m_materials(materials)
{
    try
    {
        CreateRenderPass(targets);
        CreatePipelines(pipelineCache, frameSetLayout, materialSetLayout);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanToonPrepass::~VulkanToonPrepass()
{
    DestroyHandles();
}

ScenePassId VulkanToonPrepass::Id() const
{
    return ScenePassId::ToonPrepass;
}

RenderPassIo VulkanToonPrepass::Io() const
{
    static constexpr std::array<RenderTargetId, 3> kWrites = {
        RenderTargetId::ToonLinearDepth,
        RenderTargetId::ToonMask,
        RenderTargetId::ToonDepth};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanToonPrepass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    // Cleared every frame, characters or none: the toon pass reads them either way.
    std::array<VkClearValue, 3> clearValues{};
    clearValues[0].color.float32[0] = kToonFarDepth;
    clearValues[1].color.float32[0] = 0.0f;
    clearValues[2].depthStencil = {kReverseDepthFar, 0};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(targets.ResolveIndex(RenderTargetId::ToonDepth, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.extent = frame.extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (!frame.toonDrawItems.empty())
    {
        SetViewportAndScissor(commandBuffer, frame.extent);
        const std::array<VkDescriptorSet, 1> frameSet = {frame.frameDescriptorSet};
        const VkDescriptorSet toonSet = m_materials.GetSet(frame.frameSlot);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, frameSet.data(), 0, nullptr);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 2, 1, &toonSet, 0, nullptr);
        // The opaque draws come first in the list, so the mask holds the nearest opaque surface before
        // the transparent ones add their depth.
        VkPipeline bound = VK_NULL_HANDLE;
        for (uint32_t index = 0; index < frame.toonDrawItems.size(); ++index)
        {
            const VulkanDrawItem& item = frame.toonDrawItems[index];
            const VkPipeline pipeline = m_pipelines[ToonPipelineIndex(item)];
            if (pipeline != bound)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = pipeline;
            }
            RecordToonDraw(commandBuffer, m_pipelineLayout, item, index, frame.toonExposureScale);
        }
    }
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanToonPrepass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
}

void VulkanToonPrepass::CreateRenderPass(const SceneRenderTargets& targets)
{
    std::array<VkAttachmentDescription, 3> attachments{};
    const std::array<RenderTargetId, 3> ids = {RenderTargetId::ToonLinearDepth, RenderTargetId::ToonMask, RenderTargetId::ToonDepth};
    for (size_t index = 0; index < attachments.size(); ++index)
    {
        VkAttachmentDescription& attachment = attachments[index];
        attachment.format = targets.GetFormat(ids[index]);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // The depth is only the prepass's own test; the toon pass reads the other two.
        attachment.storeOp = index == 2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // initialLayout == finalLayout: RenderTargetLayoutTracker owns the transitions.
        attachment.initialLayout = index == 2 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = attachment.initialLayout;
    }
    const std::array<VkAttachmentReference, 2> colorReferences = {
        VkAttachmentReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        VkAttachmentReference{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference depthReference{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
    subpass.pColorAttachments = colorReferences.data();
    subpass.pDepthStencilAttachment = &depthReference;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create toon prepass render pass");
}

void VulkanToonPrepass::CreatePipelines(
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout)
{
    const std::array<VkDescriptorSetLayout, 3> setLayouts = {frameSetLayout, materialSetLayout, m_materials.GetSetLayout()};
    m_pipelineLayout = CreateToonPipelineLayout(m_device, setLayouts, "Failed to create toon prepass pipeline layout");

    const std::filesystem::path shaderDir = EnginePaths::ShaderRoot();
    const VulkanShaderModule vertexShader(m_device, shaderDir / "toon.vert.spv");
    const VulkanShaderModule fragmentShader(m_device, shaderDir / "toon_prepass.frag.spv");
    for (size_t index = 0; index < m_pipelines.size(); ++index)
    {
        const bool transparent = index >= 2;
        ToonPipelineDescription description{};
        description.vertexShader = &vertexShader;
        description.fragmentShader = &fragmentShader;
        description.cullMode = (index % 2) == 1 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        description.depthCompare = kReverseDepthNearer;
        description.colorAttachmentCount = 2;
        description.singleChannel = true;
        // A transparent surface adds its depth but leaves the eye mask to the opaque ones.
        description.writeAttachment = {true, !transparent};
        m_pipelines[index] = CreateToonPipeline(
            m_device, pipelineCache, m_renderPass, m_pipelineLayout, description, "Failed to create toon prepass pipeline");
    }
}

void VulkanToonPrepass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const std::array<VkImageView, 3> attachments = {
            targets.GetView(RenderTargetId::ToonLinearDepth, slot),
            targets.GetView(RenderTargetId::ToonMask, slot),
            targets.GetView(RenderTargetId::ToonDepth, slot)};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create toon prepass framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanToonPrepass::DestroyFramebuffers()
{
    DestroyFramebufferList(m_device, m_framebuffers);
}

void VulkanToonPrepass::DestroyHandles()
{
    DestroyPipelines(m_device, m_pipelines);
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

// ---------------------------------------------------------------------------------------------
// VulkanToonPass

VulkanToonPass::VulkanToonPass(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const SceneRenderTargets& targets,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout,
    const VulkanToonMaterials& materials)
    : m_device(device),
      m_materials(materials)
{
    try
    {
        CreateRenderPass(targets);
        CreateTargetSetLayout();
        CreatePipelines(pipelineCache, frameSetLayout, materialSetLayout);
        CreateTargetSets(targets);
        CreateFramebuffers(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanToonPass::~VulkanToonPass()
{
    DestroyHandles();
}

ScenePassId VulkanToonPass::Id() const
{
    return ScenePassId::Toon;
}

RenderPassIo VulkanToonPass::Io() const
{
    static constexpr std::array<RenderTargetId, 2> kReads = {RenderTargetId::ToonLinearDepth, RenderTargetId::ToonMask};
    // The velocity too: the transparent surfaces (the front hair) are in no G-buffer, so their motion
    // goes in here for TAA and DLSS.
    static constexpr std::array<RenderTargetId, 3> kWrites = {
        RenderTargetId::SceneHdr, RenderTargetId::SceneDepth, RenderTargetId::GBufferVelocity};
    RenderPassIo io{};
    io.reads = kReads;
    io.writes = kWrites;
    return io;
}

void VulkanToonPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    if (frame.toonDrawItems.empty())
    {
        return;
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers.at(targets.ResolveIndex(RenderTargetId::SceneHdr, frame.imageIndex, frame.frameSlot));
    renderPassInfo.renderArea.extent = frame.extent;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    SetViewportAndScissor(commandBuffer, frame.extent);

    const VkDescriptorSet frameSet = frame.frameDescriptorSet;
    const VkDescriptorSet toonSet = m_materials.GetSet(frame.frameSlot);
    const VkDescriptorSet targetSet = m_targetSets.at(frame.frameSlot);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &frameSet, 0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 2, 1, &toonSet, 0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 3, 1, &targetSet, 0, nullptr);

    VkPipeline bound = VK_NULL_HANDLE;
    const auto draw = [&](VkPipeline pipeline, uint32_t index)
    {
        if (pipeline != bound)
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        RecordToonDraw(commandBuffer, m_pipelineLayout, frame.toonDrawItems[index], index, frame.toonExposureScale);
    };
    // The list is the opaque draws, then the transparent ones, each in render queue order.
    uint32_t firstTransparent = 0;
    while (firstTransparent < frame.toonDrawItems.size() && !frame.toonDrawItems[firstTransparent].toon->Has(kToonFeatureTransparent))
    {
        ++firstTransparent;
    }
    for (uint32_t index = 0; index < firstTransparent; ++index)
    {
        draw(m_pipelines[ToonPipelineIndex(frame.toonDrawItems[index])], index);
    }
    // Every outline after the opaque surfaces, as the renderer feature that draws Unity's outline
    // pass does; the transparent surfaces then cover theirs where they are nearer.
    for (uint32_t index = 0; index < frame.toonDrawItems.size(); ++index)
    {
        if (frame.toonDrawItems[index].toon->Has(kToonFeatureOutline))
        {
            draw(m_pipelines[kOutlinePipeline], index);
        }
    }
    for (uint32_t index = firstTransparent; index < frame.toonDrawItems.size(); ++index)
    {
        draw(m_pipelines[ToonPipelineIndex(frame.toonDrawItems[index])], index);
    }
    vkCmdEndRenderPass(commandBuffer);
}

void VulkanToonPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    DestroyFramebuffers();
    CreateFramebuffers(targets);
    CreateTargetSets(targets);
}

void VulkanToonPass::CreateRenderPass(const SceneRenderTargets& targets)
{
    // As the forward pass's load variant: the HDR target and the scene depth, kept in the layouts the
    // tracker put them in.
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0].format = targets.GetFormat(RenderTargetId::SceneHdr);
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = targets.GetFormat(RenderTargetId::SceneDepth);
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2] = attachments[0];
    attachments[2].format = targets.GetFormat(RenderTargetId::GBufferVelocity);

    const std::array<VkAttachmentReference, 2> colorReferences = {
        VkAttachmentReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        VkAttachmentReference{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
    subpass.pColorAttachments = colorReferences.data();
    subpass.pDepthStencilAttachment = &depthReference;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create toon render pass");
}

void VulkanToonPass::CreateTargetSetLayout()
{
    // The linear depth and the mask; the shader fetches texels by index, so they are sampled images
    // without samplers.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index)
    {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_targetSetLayout), "Failed to create toon target set layout");
}

void VulkanToonPass::CreatePipelines(
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout)
{
    const std::array<VkDescriptorSetLayout, 4> setLayouts = {
        frameSetLayout, materialSetLayout, m_materials.GetSetLayout(), m_targetSetLayout};
    m_pipelineLayout = CreateToonPipelineLayout(m_device, setLayouts, "Failed to create toon pipeline layout");

    const std::filesystem::path shaderDir = EnginePaths::ShaderRoot();
    const VulkanShaderModule vertexShader(m_device, shaderDir / "toon.vert.spv");
    const VulkanShaderModule fragmentShader(m_device, shaderDir / "toon.frag.spv");
    for (size_t index = 0; index < 4; ++index)
    {
        const bool transparent = index >= 2;
        ToonPipelineDescription description{};
        description.vertexShader = &vertexShader;
        description.fragmentShader = &fragmentShader;
        description.cullMode = (index % 2) == 1 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        // The opaque surfaces land on the depth the geometry pass wrote for them (the forward-only
        // order has none, and they write their own); the transparent ones test against everything.
        description.depthCompare = kReverseDepthNearerOrEqual;
        description.depthBias = !transparent;
        description.blend = transparent;
        m_pipelines[index] = CreateToonPipeline(
            m_device, pipelineCache, m_renderPass, m_pipelineLayout, description, "Failed to create toon pipeline");
    }
    // The outline: the hull's back faces, behind the surface wherever the surface faces the camera.
    ToonPipelineDescription outline{};
    outline.vertexShader = &vertexShader;
    outline.fragmentShader = &fragmentShader;
    outline.outline = true;
    outline.cullMode = VK_CULL_MODE_FRONT_BIT;
    outline.depthCompare = kReverseDepthNearer;
    m_pipelines[kOutlinePipeline] = CreateToonPipeline(
        m_device, pipelineCache, m_renderPass, m_pipelineLayout, outline, "Failed to create toon outline pipeline");
}

void VulkanToonPass::CreateTargetSets(const SceneRenderTargets& targets)
{
    const uint32_t copyCount = targets.GetTransientCopyCount();
    if (m_targetPool == VK_NULL_HANDLE)
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSize.descriptorCount = copyCount * 2;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = copyCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_targetPool), "Failed to create toon target descriptor pool");
    }
    else
    {
        // Called again from OnTargetsRebuilt: resetting the pool returns the previous sets to it.
        m_targetSets.clear();
        CheckVulkan(vkResetDescriptorPool(m_device, m_targetPool, 0), "Failed to reset toon target descriptor pool");
    }

    const std::vector<VkDescriptorSetLayout> layouts(copyCount, m_targetSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_targetPool;
    allocateInfo.descriptorSetCount = copyCount;
    allocateInfo.pSetLayouts = layouts.data();
    m_targetSets.assign(copyCount, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_targetSets.data()), "Failed to allocate toon target sets");

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        std::array<VkDescriptorImageInfo, 2> imageInfos{};
        imageInfos[0].imageView = targets.GetSampledView(RenderTargetId::ToonLinearDepth, slot);
        imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[1].imageView = targets.GetSampledView(RenderTargetId::ToonMask, slot);
        imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_targetSets[slot];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = &imageInfos[binding];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanToonPass::CreateFramebuffers(const SceneRenderTargets& targets)
{
    const VkExtent2D extent = targets.GetExtent();
    const uint32_t copyCount = targets.GetTransientCopyCount();
    m_framebuffers.reserve(copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        const std::array<VkImageView, 3> attachments = {
            targets.GetView(RenderTargetId::SceneHdr, slot),
            targets.GetView(RenderTargetId::SceneDepth, slot),
            targets.GetView(RenderTargetId::GBufferVelocity, slot)};
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer), "Failed to create toon framebuffer");
        m_framebuffers.push_back(framebuffer);
    }
}

void VulkanToonPass::DestroyFramebuffers()
{
    DestroyFramebufferList(m_device, m_framebuffers);
}

void VulkanToonPass::DestroyHandles()
{
    DestroyPipelines(m_device, m_pipelines);
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_targetPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_targetPool, nullptr);
        m_targetPool = VK_NULL_HANDLE;
    }
    m_targetSets.clear();
    if (m_targetSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_targetSetLayout, nullptr);
        m_targetSetLayout = VK_NULL_HANDLE;
    }
    DestroyFramebuffers();
    if (m_renderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
}
}
