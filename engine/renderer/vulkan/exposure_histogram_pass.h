#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// Counts the HDR target's pixels into a log2-luminance histogram (see exposure_histogram.comp)
// for auto exposure to meter from on the CPU. Records compute work between the forward and tone
// mapping passes and writes no render target.
//
// There is one host-visible histogram buffer per frame slot, like the transient targets it reads.
// A slot's buffer is complete once AcquireNextImage has waited on that slot's fence, which is when
// the renderer reads it: the result is kMaxFramesInFlight frames old, and the GPU never waits for
// the CPU. Buffers start zeroed, so a slot that has not run yet reads as an empty histogram.
class VulkanExposureHistogramPass : public IScenePass
{
  public:
    VulkanExposureHistogramPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets);
    ~VulkanExposureHistogramPass() override;

    VulkanExposureHistogramPass(const VulkanExposureHistogramPass&) = delete;
    VulkanExposureHistogramPass& operator=(const VulkanExposureHistogramPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The histogram the given slot last wrote. Only valid to read once that slot's fence has
    // signaled, and until the slot is recorded again.
    std::span<const uint32_t> GetHistogram(uint32_t frameSlot) const;
    // The same frame's average colour (rgb over luminance, so about 1 for a gray view), for auto
    // white balance; empty when no pixel was metered. Same validity as GetHistogram.
    std::optional<glm::vec3> GetFrameColor(uint32_t frameSlot) const;

  private:
    struct HistogramBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        nvrhi::BufferHandle handle;
        const uint32_t* mapped = nullptr;
    };

    void CreateHistogramBuffers(uint32_t count);
    void CreateBindingSets(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<HistogramBuffer> m_histograms;
};
}
