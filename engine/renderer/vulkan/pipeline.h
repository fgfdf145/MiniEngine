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
VkRenderPass CreateFullscreenRenderPass(VkDevice device, VkFormat colorFormat, const char* label);

// The pipeline every full-screen pass uses: fullscreen.vert with no vertex input, no culling, no
// depth test, dynamic viewport and scissor, and all four channels written without blending. Only
// the fragment stage, the layout and the render pass vary.
VkPipeline CreateFullscreenPipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkRenderPass renderPass,
    VkPipelineLayout layout,
    const char* fragmentShaderName,
    const char* label);
}
