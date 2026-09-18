#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

// Resolves the HDR target into the sRGB image ImGui samples. Owns its render pass, framebuffers,
// pipeline and the descriptor sets holding the HDR sampler.
//
// Its framebuffers are indexed by swapchain image because the LDR target is, while its descriptor
// sets are indexed by frame slot because the HDR target is. That split is the whole reason
// SceneRenderTargets exposes two copy counts.
//
// It also serves the G-buffer debug views: its layout is set 0 its HDR sampler, set 1 the empty
// filler, set 2 the G-buffer inputs, plus a push constant carrying the exposure and the selected
// view.
class VulkanTonemapPass : public IScenePass
{
  public:
    VulkanTonemapPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout gbufferSetLayout,
        VkDescriptorSetLayout emptySetLayout);
    ~VulkanTonemapPass() override;

    VulkanTonemapPass(const VulkanTonemapPass&) = delete;
    VulkanTonemapPass& operator=(const VulkanTonemapPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateDescriptorSetLayout();
    void CreateSampler();
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreatePipeline(VkPipelineCache pipelineCache, VkDescriptorSetLayout gbufferSetLayout, VkDescriptorSetLayout emptySetLayout);
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    // Shared by the destructor and the constructor's unwind path, the way VulkanPipelineSet does
    // it: a throw part way through construction skips the destructor, so both need the same
    // teardown and keeping one list of it is what stops the two drifting apart.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
