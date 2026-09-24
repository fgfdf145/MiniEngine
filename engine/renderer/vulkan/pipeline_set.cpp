#include "pipeline_set.h"

#include "buffer.h"
#include "pipeline.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>

#include <filesystem>

namespace me
{

namespace
{
// Per-variant state. Everything else (vertex input, input assembly, viewport/dynamic state,
// multisampling, layout, shader modules) is shared by every variant and lives in the constructor.
// These structs hold pointers into themselves, so the array below is filled in place and never
// copied or moved before vkCreateGraphicsPipelines consumes it.
struct PipelineVariantState
{
    VkBool32 alphaMaskEnabled = VK_FALSE;
    VkSpecializationMapEntry specializationEntry{};
    VkSpecializationInfo specialization{};
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    std::array<VkPipelineColorBlendAttachmentState, kMaxMaterialColorAttachments> colorBlendAttachments{};
    VkPipelineColorBlendStateCreateInfo colorBlending{};
};
}

VulkanPipelineSet::VulkanPipelineSet(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkRenderPass renderPass,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout,
    const MaterialPipelineSetConfig& config)
    : m_device(device)
{
    try
    {
        if (config.fragmentShader == nullptr ||
            config.colorAttachmentCount == 0 ||
            config.colorAttachmentCount > kMaxMaterialColorAttachments)
        {
            throw std::runtime_error("MaterialPipelineSetConfig needs a fragment shader and 1 to 5 color attachments");
        }

        const std::filesystem::path shaderDir = EnginePaths::ShaderRoot();
        const VulkanShaderModule vertexShader(m_device, shaderDir / "triangle.vert.spv");
        const VulkanShaderModule fragmentShader(m_device, shaderDir / config.fragmentShader);

        const VkVertexInputBindingDescription bindingDescription = GetVertexBindingDescription();
        const auto attributeDescriptions = GetVertexAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // The counts are still baked in, but the values come from vkCmdSetViewport/vkCmdSetScissor
        // (see VulkanForwardPass::Record) so that resizing the scene viewport only rebuilds
        // its images and framebuffers, never the pipelines.
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        const std::array<VkDynamicState, 2> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPushConstantRange pushConstantRange{};
        // Only triangle.vert reads it: the fragment shaders take their material from set 0.
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);

        const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, materialSetLayout};

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_layout), "Failed to create pipeline layout");

        std::array<PipelineVariantState, kMaterialPipelineVariantCount> variants{};
        std::array<VkGraphicsPipelineCreateInfo, kMaterialPipelineVariantCount> pipelineInfos{};

        for (MaterialAlphaMode mode : {
                 MaterialAlphaMode::Opaque,
                 MaterialAlphaMode::Mask,
                 MaterialAlphaMode::Blend})
        {
            for (bool doubleSided : {false, true})
            {
                const MaterialPipelineKey key{mode, doubleSided};
                const MaterialPipelineState state = GetMaterialPipelineState(key);
                const size_t index = GetMaterialPipelineIndex(key);
                PipelineVariantState& variant = variants[index];

                variant.alphaMaskEnabled = state.alphaMaskEnabled ? VK_TRUE : VK_FALSE;
                variant.specializationEntry.constantID = 0;
                variant.specializationEntry.offset = 0;
                variant.specializationEntry.size = sizeof(variant.alphaMaskEnabled);
                variant.specialization.mapEntryCount = 1;
                variant.specialization.pMapEntries = &variant.specializationEntry;
                variant.specialization.dataSize = sizeof(variant.alphaMaskEnabled);
                variant.specialization.pData = &variant.alphaMaskEnabled;

                variant.stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                variant.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                variant.stages[0].module = vertexShader.GetHandle();
                variant.stages[0].pName = "main";

                variant.stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                variant.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                variant.stages[1].module = fragmentShader.GetHandle();
                variant.stages[1].pName = "main";
                variant.stages[1].pSpecializationInfo = &variant.specialization;

                variant.rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                variant.rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
                variant.rasterizer.lineWidth = 1.0f;
                // Winding: Vulkan framebuffer Y points down, which alone would flip glTF's CCW front
                // faces to CW — but the render projection's Y-flip (proj[1][1] *= -1, see
                // UpdateViewportMatrices) flips them back, so front faces arrive COUNTER_CLOCKWISE in
                // framebuffer space (same combination as the classic Vulkan tutorial). Declaring
                // CLOCKWISE here culls the camera-facing side of every model. Materials flagged
                // doubleSided (glTF doubleSided=true, e.g. foliage/glass) use the no-cull variant.
                variant.rasterizer.cullMode = state.cullBackFaces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                variant.rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

                variant.depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                variant.depthStencil.depthTestEnable = VK_TRUE;
                variant.depthStencil.depthWriteEnable = state.depthWriteEnabled ? VK_TRUE : VK_FALSE;
                variant.depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
                variant.depthStencil.depthBoundsTestEnable = VK_FALSE;
                variant.depthStencil.stencilTestEnable = VK_FALSE;

                // Vulkan requires valid alpha blend enums whenever blending is enabled. Whether
                // alpha is written at all is the config's call: the forward set keeps the HDR
                // target's clear alpha, the geometry set writes every G-buffer channel.
                VkColorComponentFlags writeMask =
                    VK_COLOR_COMPONENT_R_BIT |
                    VK_COLOR_COMPONENT_G_BIT |
                    VK_COLOR_COMPONENT_B_BIT;
                if (config.writeAlpha)
                {
                    writeMask |= VK_COLOR_COMPONENT_A_BIT;
                }

                for (uint32_t attachment = 0; attachment < config.colorAttachmentCount; ++attachment)
                {
                    VkPipelineColorBlendAttachmentState& blend = variant.colorBlendAttachments[attachment];
                    blend.blendEnable = (state.blendEnabled && config.allowBlending) ? VK_TRUE : VK_FALSE;
                    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    blend.colorBlendOp = VK_BLEND_OP_ADD;
                    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    blend.alphaBlendOp = VK_BLEND_OP_ADD;
                    blend.colorWriteMask = writeMask;
                }

                variant.colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                variant.colorBlending.attachmentCount = config.colorAttachmentCount;
                variant.colorBlending.pAttachments = variant.colorBlendAttachments.data();

                VkGraphicsPipelineCreateInfo& pipelineInfo = pipelineInfos[index];
                pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pipelineInfo.stageCount = static_cast<uint32_t>(variant.stages.size());
                pipelineInfo.pStages = variant.stages.data();
                pipelineInfo.pVertexInputState = &vertexInputInfo;
                pipelineInfo.pInputAssemblyState = &inputAssembly;
                pipelineInfo.pViewportState = &viewportState;
                pipelineInfo.pRasterizationState = &variant.rasterizer;
                pipelineInfo.pMultisampleState = &multisampling;
                pipelineInfo.pDepthStencilState = &variant.depthStencil;
                pipelineInfo.pColorBlendState = &variant.colorBlending;
                pipelineInfo.pDynamicState = &dynamicState;
                pipelineInfo.layout = m_layout;
                pipelineInfo.renderPass = renderPass;
                pipelineInfo.subpass = 0;
            }
        }

        CheckVulkan(
            vkCreateGraphicsPipelines(
                m_device,
                pipelineCache,
                static_cast<uint32_t>(pipelineInfos.size()),
                pipelineInfos.data(),
                nullptr,
                m_pipelines.data()),
            "Failed to create graphics pipelines");
        LOG_INFO("Created {} material pipeline variants for {}", m_pipelines.size(), config.fragmentShader);
    }
    catch (...)
    {
        // vkCreateGraphicsPipelines may have written valid handles for the variants that did
        // succeed before it failed, so clean up whatever ended up non-null.
        DestroyHandles();
        throw;
    }
}

VulkanPipelineSet::~VulkanPipelineSet()
{
    DestroyHandles();
}

VkPipeline VulkanPipelineSet::Get(MaterialPipelineKey key) const
{
    return m_pipelines.at(GetMaterialPipelineIndex(key));
}

VkPipelineLayout VulkanPipelineSet::GetLayout() const
{
    return m_layout;
}

void VulkanPipelineSet::DestroyHandles()
{
    for (VkPipeline& pipeline : m_pipelines)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
}
}
