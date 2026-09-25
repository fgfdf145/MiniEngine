#include "shadow_pass.h"

#include "buffer.h"
#include "format_support.h"
#include "pipeline.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>

#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// Slope-scaled rasterization bias, applied when the map is rendered. It covers the depth error that
// grows with the surface's slope to the light; the shader's normal offset covers the rest.
constexpr float kDepthBiasConstant = 1.0f;
constexpr float kDepthBiasSlope = 2.0f;

uint32_t FindMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 &&
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }
    throw std::runtime_error("Failed to find a memory type for the shadow map");
}

VkFormatFeatureFlags QueryOptimalFeatures(VkPhysicalDevice physicalDevice, VkFormat format)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    return properties.optimalTilingFeatures;
}
}

VulkanShadowPass::VulkanShadowPass(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout materialSetLayout,
    uint32_t resolution)
    : m_device(device),
      m_resolution(resolution)
{
    // A throw out of a constructor skips the destructor; DestroyHandles skips null handles, so the
    // unwind path and the destructor share it.
    try
    {
        CreateImage(physicalDevice);
        CreateSampler(physicalDevice);
        CreateRenderPass();
        CreateFramebuffers();
        CreatePipelines(pipelineCache, materialSetLayout);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
    LOG_INFO("Created a {}x{} shadow map with {} cascades", m_resolution, m_resolution, kShadowCascadeCount);
}

VulkanShadowPass::~VulkanShadowPass()
{
    DestroyHandles();
}

uint32_t VulkanShadowPass::GetResolution() const
{
    return m_resolution;
}

TextureDescriptorBinding VulkanShadowPass::GetSampledBinding() const
{
    return TextureDescriptorBinding{m_arrayView, m_sampler};
}

void VulkanShadowPass::Record(
    VkCommandBuffer commandBuffer,
    std::span<const ShadowDrawItem> drawItems,
    const ShadowCascades* cascades) const
{
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount; ++cascadeIndex)
    {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_framebuffers[cascadeIndex];
        renderPassInfo.renderArea.extent = {m_resolution, m_resolution};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        if (cascades != nullptr)
        {
            const glm::mat4& lightViewProjection = (*cascades)[cascadeIndex].viewProjection;
            VkPipeline boundPipeline = VK_NULL_HANDLE;
            for (const ShadowDrawItem& item : drawItems)
            {
                if (!ShadowCascadeIntersectsSphere(lightViewProjection, item.worldBoundsCenter, item.worldBoundsRadius))
                {
                    continue;
                }

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
                constants.lightModelViewProjection = lightViewProjection * item.model;
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
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &item.vertexBuffer, &offset);
                vkCmdBindIndexBuffer(commandBuffer, item.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, item.indexCount, 1, 0, 0, 0);
            }
        }

        vkCmdEndRenderPass(commandBuffer);
    }
}

void VulkanShadowPass::CreateImage(VkPhysicalDevice physicalDevice)
{
    static constexpr std::array<VkFormat, 2> kCandidates = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    m_format = ChooseFormat(
        "shadow map",
        kCandidates,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        [physicalDevice](VkFormat format)
        {
            return QueryOptimalFeatures(physicalDevice, format);
        });

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_format;
    imageInfo.extent = {m_resolution, m_resolution, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = kShadowCascadeCount;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &m_image), "Failed to create shadow map image");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, m_image, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_memory), "Failed to allocate shadow map memory");
    CheckVulkan(vkBindImageMemory(m_device, m_image, m_memory, 0), "Failed to bind shadow map memory");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = m_format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = kShadowCascadeCount;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_arrayView), "Failed to create shadow map view");

    // One single-layer view per cascade, for that cascade's framebuffer.
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    for (uint32_t layer = 0; layer < kShadowCascadeCount; ++layer)
    {
        viewInfo.subresourceRange.baseArrayLayer = layer;
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_layerViews[layer]), "Failed to create shadow cascade view");
    }
}

void VulkanShadowPass::CreateSampler(VkPhysicalDevice physicalDevice)
{
    // With linear filtering a comparison sampler returns the bilinear blend of four comparisons,
    // which is what turns the shader's 3x3 taps into a smooth 4x4 texel filter. Not every depth
    // format supports it, so fall back to nearest where it does not.
    const bool linear =
        (QueryOptimalFeatures(physicalDevice, m_format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.minFilter = samplerInfo.magFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Outside the map counts as lit: the border is the far plane, and every receiver passes a
    // LESS_OR_EQUAL comparison against it.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.maxLod = 0.0f;
    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create shadow map sampler");
}

void VulkanShadowPass::CreateRenderPass()
{
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_format;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // Every frame rewrites the whole layer, so its previous contents are discarded.
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 0;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthReference;

    std::array<VkSubpassDependency, 2> dependencies{};
    // The previous frame's material pass may still be sampling this layer: one image serves every
    // frame in flight. A barrier's first scope covers everything submitted to the queue before it,
    // so this execution dependency orders the clear after those reads. It is a write after read,
    // so no memory dependency is needed.
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // And this frame's material pass reads what was written here.
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();
    CheckVulkan(vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass), "Failed to create shadow render pass");
}

void VulkanShadowPass::CreateFramebuffers()
{
    for (uint32_t layer = 0; layer < kShadowCascadeCount; ++layer)
    {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_layerViews[layer];
        framebufferInfo.width = m_resolution;
        framebufferInfo.height = m_resolution;
        framebufferInfo.layers = 1;
        CheckVulkan(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[layer]), "Failed to create shadow framebuffer");
    }
}

void VulkanShadowPass::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout materialSetLayout)
{
    const std::filesystem::path shaderDir = EnginePaths::ShaderRoot();
    const VulkanShaderModule vertexShader(m_device, shaderDir / "shadow.vert.spv");
    const VulkanShaderModule fragmentShader(m_device, shaderDir / "shadow.frag.spv");

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(ShadowPushConstants);

    // The material set sits at set 0 here: the pass binds no camera set.
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &materialSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout), "Failed to create shadow pipeline layout");

    // Only position and texture coordinate are read, so only those two attributes are declared.
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

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // The resolution never changes, so viewport and scissor are baked in.
    VkViewport viewport{};
    viewport.width = static_cast<float>(m_resolution);
    viewport.height = static_cast<float>(m_resolution);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {m_resolution, m_resolution};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // No culling: single-sided geometry such as a wall with one face still has to block the light
    // from behind, and the depth bias rather than front-face culling keeps acne off lit surfaces.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = kDepthBiasConstant;
    rasterizer.depthBiasSlopeFactor = kDepthBiasSlope;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

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

    // Opaque casters need no fragment shader: depth is all the pass writes.
    std::array<VkGraphicsPipelineCreateInfo, 2> pipelineInfos{};
    for (VkGraphicsPipelineCreateInfo& pipelineInfo : pipelineInfos)
    {
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
    }
    pipelineInfos[0].stageCount = 1;
    pipelineInfos[1].stageCount = 2;

    std::array<VkPipeline, 2> pipelines{};
    const VkResult result = vkCreateGraphicsPipelines(
        m_device,
        pipelineCache,
        static_cast<uint32_t>(pipelineInfos.size()),
        pipelineInfos.data(),
        nullptr,
        pipelines.data());
    // Assigned before the check so a partial failure still leaves every created handle reachable
    // by DestroyHandles.
    m_opaquePipeline = pipelines[0];
    m_maskPipeline = pipelines[1];
    CheckVulkan(result, "Failed to create shadow pipelines");
}

void VulkanShadowPass::DestroyHandles()
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
    for (VkFramebuffer& framebuffer : m_framebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
            framebuffer = VK_NULL_HANDLE;
        }
    }
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
    for (VkImageView& view : m_layerViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (m_arrayView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_arrayView, nullptr);
        m_arrayView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }
}
}
