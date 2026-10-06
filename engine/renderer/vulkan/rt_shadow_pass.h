#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <array>
#include <cstdint>
#include <vector>

namespace me
{

class VulkanRayScene;

// The sun's shadow traced with hardware ray queries (docs/design/2026-10-07-ray-traced-effects-design.md),
// in place of the cascades for every surface the lighting pass shades. Three dispatches between the
// geometry and lighting passes:
//   trace (rt_shadow_trace.comp): one ray a pixel toward a point on the sun's disk, through the ray
//     scene with alpha-tested surfaces tested against their textures; ShadowRaw holds the visibility
//     and the occluder's distance;
//   temporal (rt_shadow_temporal.comp): accumulation reprojected through the motion vectors, the
//     history clamped to the raw neighbourhood so a moving shadow leaves no trail;
//   filter (rt_shadow_filter.comp): a depth- and normal-aware blur as wide as the penumbra the
//     occluder's distance and the sun's size make, into SceneShadow (r visibility, g 1 where it ran).
// With RayTracingSettings::denoise off the filter passes the raw trace through. While the traced
// shadow does not run the filter alone records, writing (1, 0) so its readers never see undefined
// contents.
class VulkanRtShadowPass : public IScenePass
{
  public:
    VulkanRtShadowPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        const VulkanRayScene& rayScene);
    ~VulkanRtShadowPass() override;

    VulkanRtShadowPass(const VulkanRtShadowPass&) = delete;
    VulkanRtShadowPass& operator=(const VulkanRtShadowPass&) = delete;

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

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    // One set for all three shaders: 0 depth, 1 G-buffer normal, 2 motion vectors, 3 ShadowRaw
    // (storage), 4 history read (sampled), 5 history write (storage), 6 SceneShadow (storage),
    // 7 the history just written (storage, read by the filter).
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Frame set, ray set, this pass's set, ray texture table (the trace); frame set and this pass's set
    // (the other two), both with the same push constants.
    VkPipelineLayout m_tracePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_tracePipeline = VK_NULL_HANDLE;
    VkPipeline m_temporalPipeline = VK_NULL_HANDLE;
    VkPipeline m_filterPipeline = VK_NULL_HANDLE;
    HistoryImagePair m_history;
    // Indexed by transient copy * 2 + history read index.
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
