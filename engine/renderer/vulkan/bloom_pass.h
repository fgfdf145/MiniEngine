#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <glm/glm.hpp>

#include <vector>

namespace me
{

// Bloom (shaders/vulkan/bloom.comp): downsamples SceneTaa through a mip chain, upsamples it back,
// and mixes the glow into SceneTaa in place, before the exposure histogram and tone mapping read
// it. The chain is one image with a level per BuildBloomMipChain entry, owned here, in GENERAL,
// rewritten every frame. Records nothing when bloom is off.
class VulkanBloomPass : public IScenePass
{
  public:
    VulkanBloomPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanBloomPass() override;

    VulkanBloomPass(const VulkanBloomPass&) = delete;
    VulkanBloomPass& operator=(const VulkanBloomPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateChain(VkExtent2D extent);
    void DestroyChain();
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    std::vector<glm::uvec2> m_levelExtents;
    VkImage m_chainImage = VK_NULL_HANDLE;
    VkDeviceMemory m_chainMemory = VK_NULL_HANDLE;
    std::vector<VkImageView> m_levelViews;

    // Per frame slot: SceneTaa into level 0, and level 0 back into SceneTaa.
    std::vector<VkDescriptorSet> m_firstDownsampleSets;
    std::vector<VkDescriptorSet> m_compositeSets;
    // Index i - 1: level i - 1 into level i.
    std::vector<VkDescriptorSet> m_downsampleSets;
    // Index i: level i + 1 added into level i.
    std::vector<VkDescriptorSet> m_upsampleSets;
};
}
