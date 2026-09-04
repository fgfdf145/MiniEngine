#pragma once

#include "common.h"

#include <engine/renderer/material_pipeline.h>

#include <array>

namespace me
{

// Owns every material pipeline variant plus the state they share. The variants differ only in
// their blend / depth-write / cull / alpha-mask flags, so they are built from a single pair of
// shader modules, share one VkPipelineLayout, and are created in a single
// vkCreateGraphicsPipelines call. The pipeline cache is owned by the renderer and outlives this
// set, so a rebuild reuses the driver's earlier shader compilation.
//
// Viewport and scissor are dynamic state, set at record time, and the descriptor set layout is
// the renderer's fixed material layout. The pipelines therefore depend on nothing but the render
// pass: they survive both a scene-viewport resize and a scene content reload untouched.
class VulkanPipelineSet
{
  public:
    VulkanPipelineSet(
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkRenderPass renderPass,
        VkDescriptorSetLayout descriptorSetLayout);
    ~VulkanPipelineSet();

    VulkanPipelineSet(const VulkanPipelineSet&) = delete;
    VulkanPipelineSet& operator=(const VulkanPipelineSet&) = delete;

    VkPipeline Get(MaterialPipelineKey key) const;
    VkPipelineLayout GetLayout() const;

  private:
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::array<VkPipeline, kMaterialPipelineVariantCount> m_pipelines{};
};
}
