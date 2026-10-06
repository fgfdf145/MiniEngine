#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <vector>

namespace me
{

// Temporal anti-aliasing resolve (shaders/vulkan/taa_resolve.comp). Reads SceneHdr, depth and the
// motion vectors, and blends them with last frame's result into SceneTaa, which the exposure
// histogram and tone mapping read. Its two history images live here, outside the layout tracker,
// like the AO resolve's; which is read and whether it is valid arrive in the frame context. With TAA
// off, and in the forward-only order, it copies SceneHdr to SceneTaa unchanged.
//
// When the frame context carries DLSS, DLSS resolves instead, from the render size to the output
// size: this pass turns the G-buffer's motion vectors into the ones DLSS reads
// (dlss_motion_vectors.comp), evaluates DLSS into SceneTaa, and copies the result into the history
// image the SSR trace reads next frame, as the TAA resolve writes it.
class VulkanTaaPass : public IScenePass
{
  public:
    VulkanTaaPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanTaaPass() override;

    VulkanTaaPass(const VulkanTaaPass&) = delete;
    VulkanTaaPass& operator=(const VulkanTaaPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // One of the two history images (last frame's anti-aliased result is at the frame's
    // taaHistory.readIndex), for the SSR trace, which takes its reflected colour from it. The
    // images are recreated in OnTargetsRebuilt, so a reader must rebuild its descriptors after
    // this pass has.
    VkImageView GetHistoryView(uint32_t index) const;

  private:
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void CreateMotionImage(VkExtent2D extent);
    void DestroyMotionImage();
    void RecordDlss(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const;
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
    // Indexed by frameSlot * 2 + readIndex: set r samples history r and stores to history 1 - r.
    std::vector<VkDescriptorSet> m_descriptorSets;

    // DLSS's motion vectors (RG16F, render size) and the pass that writes them, one set per frame slot.
    VkDescriptorSetLayout m_motionSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_motionDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_motionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_motionPipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_motionDescriptorSets;
    VkImage m_motionImage = VK_NULL_HANDLE;
    VkDeviceMemory m_motionMemory = VK_NULL_HANDLE;
    VkImageView m_motionView = VK_NULL_HANDLE;
};
}
