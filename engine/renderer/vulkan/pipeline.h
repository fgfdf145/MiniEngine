#pragma once

#include "common.h"

#include <filesystem>

namespace me
{

// RAII wrapper around a VkShaderModule loaded from a SPIR-V file. Modules are only needed while
// vkCreateGraphicsPipelines runs, so VulkanPipelineSet loads each stage once, builds every
// material variant from it, and lets the module go out of scope afterwards.
class VulkanShaderModule
{
  public:
    VulkanShaderModule(VkDevice device, const std::filesystem::path& path);
    ~VulkanShaderModule();

    VulkanShaderModule(const VulkanShaderModule&) = delete;
    VulkanShaderModule& operator=(const VulkanShaderModule&) = delete;

    VkShaderModule GetHandle() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkShaderModule m_module = VK_NULL_HANDLE;
};

// The render pass every full-screen pass uses: one color attachment of the given format, not
// cleared because the triangle covers every pixel, stored, and initialLayout == finalLayout ==
// COLOR_ATTACHMENT_OPTIMAL so RenderTargetLayoutTracker stays the only authority on layouts.
// label names the pass in the failure message.
// loadOp DONT_CARE for a pass that writes every pixel; LOAD for one that blends over them.
VkRenderPass CreateFullscreenRenderPass(
    VkDevice device,
    VkFormat colorFormat,
    const char* label,
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE);

// What varies between full-screen pipelines beyond the fragment stage.
struct FullscreenPipelineOptions
{
    const char* vertexShaderName = "fullscreen.vert.spv";
    // Test against the pass's depth attachment with LESS_OR_EQUAL, writing nothing: with sky.vert,
    // which places the triangle at depth 1, that draws only where no geometry did.
    bool depthTestAtFarPlane = false;
    // Adds the fragment's rgb to what the attachment holds (ONE, ONE) and leaves alpha alone.
    bool additiveBlend = false;
    // Test with LESS and write the depth the fragment shader gives (gl_FragDepth), as a surface drawn
    // among the scene's geometry does. Overrides depthTestAtFarPlane.
    bool depthTestAndWrite = false;
    // The render pass's color attachments, each written like the first.
    uint32_t colorAttachmentCount = 1;
};

// The pipeline every full-screen pass uses: fullscreen.vert with no vertex input, no culling, no
// depth test unless options ask for it, dynamic viewport and scissor, and all four channels written without blending. Only
// the fragment stage, the layout and the render pass vary.
VkPipeline CreateFullscreenPipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkRenderPass renderPass,
    VkPipelineLayout layout,
    const char* fragmentShaderName,
    const char* label,
    const FullscreenPipelineOptions& options = {});
}
