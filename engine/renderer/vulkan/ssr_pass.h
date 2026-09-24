#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <cstdint>
#include <vector>

namespace me
{

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
    VulkanSsrTracePass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        const VulkanTaaPass& taa);
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
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    const VulkanTaaPass& m_taa;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    // Indexed by frameSlot * 2 + the TAA history index that holds last frame's image.
    std::vector<VkDescriptorSet> m_descriptorSets;
};

// Screen-space reflections, the resolve (shaders/vulkan/ssr_resolve.comp): a roughness-sized spatial
// filter and temporal accumulation into SceneReflections, which the lighting pass reads. Always
// records, so SceneReflections is defined whatever the settings.
class VulkanSsrResolvePass : public IScenePass
{
  public:
    VulkanSsrResolvePass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
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
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    HistoryImagePair m_history;
    // Indexed by frameSlot * 2 + readIndex: set r samples m_history[r] and stores to m_history[1 - r].
    std::vector<VkDescriptorSet> m_descriptorSets;
};

// Whether the trace runs this frame: SSR on and last frame's anti-aliased image available.
bool SsrTraces(const ScenePassFrameContext& frame);
}
