#pragma once

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
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
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
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    const VulkanRayScene& m_rayScene;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    // ddgi_debug_ray_query.comp, when the ray scene has hardware ray tracing.
    VkPipeline m_rayQueryPipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
