#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"

#include <vector>

namespace me
{

// Resolves the HDR target into the sRGB image ImGui samples: an NVRHI graphics pipeline drawn with
// NVRHI's dynamic rendering into a framebuffer per LDR copy.
//
// Its framebuffers are indexed by swapchain image because the LDR target is, while its binding sets
// are indexed by frame slot because the HDR target is. That split is the whole reason
// SceneRenderTargets exposes two copy counts.
//
// It also serves the G-buffer debug views: set 0 its HDR texture and sampler, set 2 the G-buffer
// inputs (NVRHI fills set 1 with an empty layout), plus a push constant carrying the exposure and
// the selected view.
class VulkanTonemapPass : public IScenePass
{
  public:
    VulkanTonemapPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* gbufferSetLayout);
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
    void CreateBindingSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};
}
