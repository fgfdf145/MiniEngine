#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <glm/glm.hpp>

#include <vector>

namespace me
{

// Bloom (shaders/vulkan/bloom.comp): downsamples SceneTaa through a mip chain, upsamples it back,
// and mixes the glow into SceneTaa in place, before the exposure histogram and tone mapping read
// it. The chain is one image with a level per BuildBloomMipChain entry, owned here and rewritten
// every frame. Records through NVRHI (NvrhiPassScope): SceneTaa comes and goes in GENERAL, as the
// native passes around it hold it. Records nothing when bloom is off.
class VulkanBloomPass : public IScenePass
{
  public:
    VulkanBloomPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanBloomPass() override;

    VulkanBloomPass(const VulkanBloomPass&) = delete;
    VulkanBloomPass& operator=(const VulkanBloomPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateChain(VkExtent2D extent);
    void CreateBindingSets(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;

    std::vector<glm::uvec2> m_levelExtents;
    // Between frames in ShaderResource (keepInitialState): NVRHI moves its levels between reads and
    // writes as the dispatches go.
    nvrhi::TextureHandle m_chainTexture;

    // Per transient copy: SceneTaa into level 0, and level 0 back into SceneTaa.
    std::vector<nvrhi::BindingSetHandle> m_firstDownsampleSets;
    std::vector<nvrhi::BindingSetHandle> m_compositeSets;
    // Index i - 1: level i - 1 into level i.
    std::vector<nvrhi::BindingSetHandle> m_downsampleSets;
    // Index i: level i + 1 added into level i.
    std::vector<nvrhi::BindingSetHandle> m_upsampleSets;
};
}
