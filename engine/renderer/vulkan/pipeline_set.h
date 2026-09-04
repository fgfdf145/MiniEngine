#pragma once

#include "pipeline.h"

#include <array>
#include <memory>

namespace me
{

class VulkanPipelineSet
{
  public:
    VulkanPipelineSet(
        VkDevice device,
        VkExtent2D extent,
        VkRenderPass renderPass,
        VkDescriptorSetLayout descriptorSetLayout);

    const VulkanPipeline& Get(MaterialPipelineKey key) const;

  private:
    std::array<std::unique_ptr<VulkanPipeline>, kMaterialPipelineVariantCount> m_pipelines;
};
}
