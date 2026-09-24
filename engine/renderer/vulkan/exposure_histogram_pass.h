#pragma once

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
    VulkanExposureHistogramPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets);
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
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const uint32_t* mapped = nullptr;
    };

    void CreateDescriptorSetLayout();
    void CreateSampler();
    void CreatePipeline(VkPipelineCache pipelineCache);
    void CreateHistogramBuffers(uint32_t count);
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    // Shared by the destructor and the constructor's unwind path, as in the other passes.
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
    std::vector<HistogramBuffer> m_histograms;
};
}
