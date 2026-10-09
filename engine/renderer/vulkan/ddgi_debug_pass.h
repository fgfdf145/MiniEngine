#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"

#include <vector>

namespace me
{

class VulkanRayScene;

// The DDGI debug views (ddgi_debug.comp): written over SceneGi after the one-bounce composite has
// added it to the image, so the tone mapping pass shows them where it shows the indirect diffuse.
// Records nothing unless one of its views is selected and the ray scene is ready.
class VulkanDdgiDebugPass : public IScenePass
{
  public:
    VulkanDdgiDebugPass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        const VulkanRayScene& rayScene);
    ~VulkanDdgiDebugPass() override;

    VulkanDdgiDebugPass(const VulkanDdgiDebugPass&) = delete;
    VulkanDdgiDebugPass& operator=(const VulkanDdgiDebugPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateBindingSets(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    const VulkanRayScene& m_rayScene;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    // ddgi_debug_ray_query.comp, when the ray scene has hardware ray tracing.
    nvrhi::ComputePipelineHandle m_rayQueryPipeline;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};
}
