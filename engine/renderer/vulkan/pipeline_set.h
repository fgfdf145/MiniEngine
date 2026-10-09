#pragma once

#include "common.h"
#include <engine/renderer/material_pipeline.h>

#include <nvrhi/nvrhi.h>

#include <array>

namespace me
{

// Owns every material pipeline variant plus the state they share. The variants differ only in
// their blend / depth-write / cull / alpha-mask flags, so they are built from one vertex shader and
// one fragment shader specialised per variant, over the same binding layouts: the frame set (set 0),
// the material set (set 1) and the draw's push constants (register space 2).
//
// The viewport is set per draw state, and the binding layouts are the renderer's fixed frame and
// material layouts. The pipelines therefore depend on nothing but the attachments' formats: they
// survive both a scene-viewport resize and a scene content reload untouched.
inline constexpr uint32_t kMaxMaterialColorAttachments = 8;

// What differs between the pipeline sets that draw material items. Everything else (vertex
// shader, vertex input, binding layouts, push constants, depth and cull policy per variant) is
// shared by construction.
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
    // VulkanPathTraceLayerPass's framebuffers: 1 writes the fragment's depth, kept by a MAX blend on
    // every variant, depth tested (nearer or equal) against the scene's but not written; 2 writes the
    // G-buffer of the fragment at that depth, with no depth attachment. Constant 3, kBlendItem, says
    // which variants draw Blend items, for every set.
    int32_t layerPass = 0;
};

// The vertex input every material pipeline reads (triangle.vert, toon.vert): the vertex at binding 0
// (locations 0 to 6, the outline normal at 6, which triangle.vert leaves unread) and where each vertex
// was last frame at binding 1 (location 7). D3D12 matches them by semantic name.
nvrhi::InputLayoutHandle CreateMaterialInputLayout(nvrhi::IDevice* device, nvrhi::IShader* vertexShader);

// The draws' push constants (ObjectPushConstants, triangle.vert's DrawConstants) in register space 2, a
// layout of their own, and the one binding set naming them.
struct MaterialDrawConstants
{
    explicit MaterialDrawConstants(nvrhi::IDevice* device);
    nvrhi::BindingLayoutHandle layout;
    nvrhi::BindingSetHandle set;
};

class VulkanPipelineSet
{
  public:
    VulkanPipelineSet(
        nvrhi::IDevice* device,
        const nvrhi::FramebufferInfo& framebuffer,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout,
        nvrhi::IBindingLayout* drawConstantsLayout,
        const MaterialPipelineSetConfig& config);
    ~VulkanPipelineSet();

    VulkanPipelineSet(const VulkanPipelineSet&) = delete;
    VulkanPipelineSet& operator=(const VulkanPipelineSet&) = delete;

    nvrhi::IGraphicsPipeline* Get(MaterialPipelineKey key) const;

  private:
    std::array<nvrhi::GraphicsPipelineHandle, kMaterialPipelineVariantCount> m_pipelines{};
};
}
