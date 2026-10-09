#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <cstdint>
#include <vector>

namespace me
{

class VulkanRayScene;
class VulkanTaaPass;

// Screen-space reflections, the trace (shaders/vulkan/ssr_trace.comp). One GGX-sampled ray a pixel
// against the depth buffer; a hit takes its colour from TAA's history, last frame's anti-aliased
// image. Writes SsrRaw. Records nothing when SSR is off or that history is not valid (TAA off, the
// first frame after a rebuild); the resolve then writes zero confidence.
class VulkanSsrTracePass : public IScenePass
{
  public:
    // taa must outlive this pass and must have rebuilt its history before OnTargetsRebuilt here,
    // which the renderer guarantees by owning TAA before SSR in its pass list.
    // With hardware ray tracing (rayScene's) the pass also makes the ray traced variant
    // (rt_reflection_trace.comp), which replaces the march where RayTracingSettings::reflections says.
    VulkanSsrTracePass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        const VulkanTaaPass& taa,
        const VulkanRayScene& rayScene);
    ~VulkanSsrTracePass() override;

    VulkanSsrTracePass(const VulkanSsrTracePass&) = delete;
    VulkanSsrTracePass& operator=(const VulkanSsrTracePass&) = delete;

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
    const VulkanTaaPass& m_taa;
    nvrhi::SamplerHandle m_nearestSampler;
    nvrhi::SamplerHandle m_linearSampler;
    // The march's set is set 1; the ray traced variant (rt_reflection_trace.comp) has the same bindings
    // at set 2, after the ray set, with the ray texture table at 3.
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    nvrhi::BindingLayoutHandle m_tracedSetLayout;
    nvrhi::ComputePipelineHandle m_tracedPipeline;
    // Indexed by frameSlot * 2 + the TAA history index that holds last frame's image.
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<nvrhi::BindingSetHandle> m_tracedBindingSets;
};

// Screen-space reflections, the resolve (shaders/vulkan/ssr_resolve.comp): a roughness-sized spatial
// filter and temporal accumulation into SceneReflections, which the lighting pass reads. Always
// records, so SceneReflections is defined whatever the settings.
class VulkanSsrResolvePass : public IScenePass
{
  public:
    VulkanSsrResolvePass(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanSsrResolvePass() override;

    VulkanSsrResolvePass(const VulkanSsrResolvePass&) = delete;
    VulkanSsrResolvePass& operator=(const VulkanSsrResolvePass&) = delete;

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
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    HistoryImagePair m_history;
    // Indexed by frameSlot * 2 + readIndex: set r samples m_history[r] and stores to m_history[1 - r].
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};

// Whether the trace runs this frame: SSR on and last frame's anti-aliased image available.
bool SsrTraces(const ScenePassFrameContext& frame);
}
