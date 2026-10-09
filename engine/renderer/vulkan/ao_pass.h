#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <array>
#include <cstdint>
#include <vector>

namespace me
{

class VulkanRayScene;

// The visibility bitmask AO trace. Reads depth and the G-buffer geometric normal, writes the noisy
// per-pixel visibility into AoRaw's r. With hardware ray tracing (RayTracingSettings) the occlusion is
// traced through the ray scene instead (rt_occlusion.comp): r the AO within the radius, unless the
// bitmask still gives it, and g the DDGI probe occlusion, the share of rays that escape to a coarse
// level's spacing (1 where the finest level answers). Records nothing when neither runs; the resolve
// then ignores AoRaw.
class VulkanAoTracePass : public IScenePass
{
  public:
    // rayScene's layouts make the traced variant's pipeline when it has hardware ray tracing.
    VulkanAoTracePass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        const VulkanRayScene& rayScene);
    ~VulkanAoTracePass() override;

    VulkanAoTracePass(const VulkanAoTracePass&) = delete;
    VulkanAoTracePass& operator=(const VulkanAoTracePass&) = delete;

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
    // The bitmask's set is set 1; the traced variant's the same bindings at set 2 (after the ray
    // set), with the ray texture table at 3. A layout names its set, so each has its own.
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    nvrhi::BindingLayoutHandle m_tracedSetLayout;
    nvrhi::ComputePipelineHandle m_tracedPipeline;
    std::vector<nvrhi::BindingSetHandle> m_tracedBindingSets;
};

// Spatial filter plus temporal accumulation into SceneAo. The two history images live here rather
// than in SceneRenderTargets because they must survive across frames, which the frame-scoped layout
// tracker cannot describe. They stay in VK_IMAGE_LAYOUT_GENERAL, and a barrier at the head of
// Record orders last frame's accesses against this frame's; the dispatch reads the history it
// samples in SHADER_READ_ONLY_OPTIMAL and puts it back. Which image is read, which is written and
// whether the read one is valid arrive in the frame context (see TemporalHistory), so the pass keeps
// no per-frame state. With AO disabled it still records, writing 1.0 to SceneAo, so the lighting
// pass and the debug view never read undefined contents.
class VulkanAoResolvePass : public IScenePass
{
  public:
    VulkanAoResolvePass(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanAoResolvePass() override;

    VulkanAoResolvePass(const VulkanAoResolvePass&) = delete;
    VulkanAoResolvePass& operator=(const VulkanAoResolvePass&) = delete;

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
    // Indexed by frameSlot * 2 + readIndex: set r samples m_history[r] and stores to
    // m_history[1 - r].
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};
}
