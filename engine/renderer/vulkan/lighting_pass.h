#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// Resolves the G-buffer into HDR radiance with one full-screen triangle: decodes GB0-GB3,
// reconstructs world position from depth, and runs pbr_common.glsl's ShadeSurface, the arithmetic
// triangle.frag also runs, directional shadow included. Pixels no geometry wrote resolve to the
// background radiance, pushed as a constant.
//
// Pipeline layout: set 0 the camera and shadow map (the frame set layout the material pipelines
// use), set 1 the empty filler, set 2 the G-buffer inputs, and a 16-byte fragment push constant;
// see VulkanGBufferDescriptors for why set 1 is empty. The shadow map is not a render target and
// is absent from Io(): VulkanShadowPass orders itself against every fragment shader read.
// Framebuffers are indexed by frame slot because the HDR target is transient.
class VulkanLightingPass : public IScenePass
{
  public:
    VulkanLightingPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout emptySetLayout,
        VkDescriptorSetLayout gbufferSetLayout);
    ~VulkanLightingPass() override;

    VulkanLightingPass(const VulkanLightingPass&) = delete;
    VulkanLightingPass& operator=(const VulkanLightingPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreatePipeline(
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout emptySetLayout,
        VkDescriptorSetLayout gbufferSetLayout);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    // Shared by the destructor and the constructor's unwind path, as in every pass.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
