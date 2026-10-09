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
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
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
    void CreateBindingSets(const SceneRenderTargets& targets);

    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_linearSampler;
    // One set's bindings for all three shaders: 0 depth, 1 G-buffer normal, 2 motion vectors, 3 ShadowRaw
    // (storage), 4 history read (sampled), 5 history write (storage), 6 SceneShadow (storage), 7 the
    // history just written (storage, read by the filter), with the push constants. The trace has it at
    // set 2 (after the ray set, the texture table at 3), the temporal pass and the filter at set 1: a
    // layout names its set, so each has its own layout and binding sets.
    nvrhi::BindingLayoutHandle m_traceSetLayout;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_tracePipeline;
    nvrhi::ComputePipelineHandle m_temporalPipeline;
    nvrhi::ComputePipelineHandle m_filterPipeline;
    HistoryImagePair m_history;
    // Indexed by transient copy * 2 + history read index.
    std::vector<nvrhi::BindingSetHandle> m_traceBindingSets;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};
}
