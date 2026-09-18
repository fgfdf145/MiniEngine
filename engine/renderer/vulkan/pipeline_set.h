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
// Viewport and scissor are dynamic state, set at record time, and the descriptor set layouts are
// the renderer's fixed frame and material layouts. The pipelines therefore depend on nothing but
// the render pass: they survive both a scene-viewport resize and a scene content reload untouched.
inline constexpr uint32_t kMaxMaterialColorAttachments = 4;

// What differs between the pipeline sets that draw material items. Everything else (vertex
// shader, vertex input, descriptor set layouts, push constants, depth and cull policy per
// variant) is shared by construction.
struct MaterialPipelineSetConfig
{
    // The compiled fragment stage, relative to EnginePaths::ShaderRoot().
    const char* fragmentShader = "triangle.frag.spv";
    uint32_t colorAttachmentCount = 1;
    // The forward pass writes RGB only and leaves the HDR target's alpha at its clear value; the
    // geometry pass owns every channel of every G-buffer target.
    bool writeAlpha = false;
    // False for the geometry pass. Blend items never reach it, but the set still builds all six
    // variants so GetMaterialPipelineIndex needs no second mapping; turning blending off keeps
    // those unused variants from declaring blend state against G-buffer attachments.
    bool allowBlending = true;
};

class VulkanPipelineSet
{
  public:
    VulkanPipelineSet(
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkRenderPass renderPass,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout materialSetLayout,
        const MaterialPipelineSetConfig& config);
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
