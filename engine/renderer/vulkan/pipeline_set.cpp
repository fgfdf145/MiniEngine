#include "pipeline_set.h"

namespace me
{

VulkanPipelineSet::VulkanPipelineSet(
    VkDevice device,
    VkExtent2D extent,
    VkRenderPass renderPass,
    VkDescriptorSetLayout descriptorSetLayout)
{
    for (MaterialAlphaMode mode : {
             MaterialAlphaMode::Opaque,
             MaterialAlphaMode::Mask,
             MaterialAlphaMode::Blend})
    {
        for (bool doubleSided : {false, true})
        {
            const MaterialPipelineKey key{mode, doubleSided};
            m_pipelines[GetMaterialPipelineIndex(key)] = std::make_unique<VulkanPipeline>(
                device,
                extent,
                renderPass,
                descriptorSetLayout,
                key);
        }
    }
}

const VulkanPipeline& VulkanPipelineSet::Get(MaterialPipelineKey key) const
{
    return *m_pipelines.at(GetMaterialPipelineIndex(key));
}
}
