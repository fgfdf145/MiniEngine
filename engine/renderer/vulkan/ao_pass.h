#pragma once

#include "scene_pass.h"

#include <array>
#include <cstdint>
#include <vector>

namespace me
{

// The visibility bitmask AO trace. Reads depth and the G-buffer geometric normal, writes the noisy
// per-pixel visibility into AoRaw. Records nothing when AO is disabled; the resolve then ignores
// AoRaw.
class VulkanAoTracePass : public IScenePass
{
  public:
    VulkanAoTracePass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
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
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};

// Spatial filter plus temporal accumulation into SceneAo. The two history images live here rather
// than in SceneRenderTargets because they must survive across frames, which the frame-scoped layout
// tracker cannot describe. They stay in VK_IMAGE_LAYOUT_GENERAL, and a barrier at the head of
// Record orders last frame's accesses against this frame's. Which image is read, which is written
// and whether the read one is valid arrive in the frame context (see AoHistory), so the pass keeps
// no per-frame state. With AO disabled it still records, writing 1.0 to SceneAo, so the lighting
// pass and the debug view never read undefined contents.
class VulkanAoResolvePass : public IScenePass
{
  public:
    VulkanAoResolvePass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
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
    struct HistoryImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void CreateHistoryImages(VkExtent2D extent);
    void DestroyHistoryImages();
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::array<HistoryImage, 2> m_history{};
    // Indexed by frameSlot * 2 + readIndex: set r samples m_history[r] and stores to
    // m_history[1 - r].
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
