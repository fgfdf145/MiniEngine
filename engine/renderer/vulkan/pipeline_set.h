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
inline constexpr uint32_t kMaxMaterialColorAttachments = 8;

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
    // Named for conventional depth; the scene's is reverse-Z, so false is GREATER and true
    // GREATER_OR_EQUAL (reverse_depth.h). Strict for the geometry pass; the forward pass takes equal
    // too: its forward-shaded opaque draws land on depth the geometry pass already wrote for them,
    // from the same vertex shader.
    bool depthLessOrEqual = true;
    // triangle.frag's kScatterPrepass (constant 1): the scatter pre-pass's set, which writes the light
    // entering the surface and the draw slot instead of the shaded colour.
    bool scatterPrepass = false;
    // gbuffer.frag's kDecal (constant 2), against the geometry pass: blended by the base colour's
    // alpha into albedo (rgb), GB2's metallic and roughness and emission, every other channel
    // masked, depth tested (nearer or equal) but not written.
    bool decal = false;
    // gbuffer.frag's two path traced layer variants (PATH_TRACE_LAYER_PASS), against
    // VulkanPathTraceLayerPass's render passes: 1 writes the fragment's depth, kept by a MAX blend on
    // every variant, depth tested (nearer or equal) against the scene's but not written; 2 writes the
    // G-buffer of the fragment at that depth, with no depth attachment. Constant 3, kBlendItem, says
    // which variants draw Blend items, for every set.
    int32_t layerPass = 0;
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
