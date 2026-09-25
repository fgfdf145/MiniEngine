#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"
#include "uniform_buffer.h"

#include <vector>

namespace me
{

// The scene behind transmissive surfaces (KHR_materials_transmission): a fixed 1024 x 1024 RGBA16F
// image with its full mip chain, as the Khronos glTF Sample Viewer keeps it, so the LOD rule in
// transmission_common.glsl is the viewer's and the frame descriptor that binds it (set 0, binding
// 18) never changes on a resize. Device lifetime. It rests in SHADER_READ_ONLY_OPTIMAL, which the
// forward pipelines' descriptor names; only VulkanTransmissionCopyPass moves it out and back.
class VulkanTransmissionImage
{
  public:
    static constexpr uint32_t kSize = 1024;
    static constexpr uint32_t kMipLevels = 11; // 1024 down to 1

    VulkanTransmissionImage(VkPhysicalDevice physicalDevice, VkDevice device);
    ~VulkanTransmissionImage();

    VulkanTransmissionImage(const VulkanTransmissionImage&) = delete;
    VulkanTransmissionImage& operator=(const VulkanTransmissionImage&) = delete;

    // Every mip, linear, clamped: what triangle.frag samples.
    TextureDescriptorBinding GetSampledBinding() const;
    VkImage GetImage() const;
    VkImageView GetLevel0View() const;

    // Moves a new image from UNDEFINED to its resting layout; later calls do nothing. Recorded by
    // the copy pass every frame, so the descriptor names the true layout from the first frame on.
    void RecordInitialTransition(VkCommandBuffer commandBuffer) const;

  private:
    void Destroy();

    VkDevice m_device = VK_NULL_HANDLE;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkImageView m_level0View = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    mutable bool m_initialized = false;
};

// Copies the HDR target, once everything opaque and the sky are in it, into the transmission image
// and builds its mip chain. Does nothing on a frame without transmissive draws: the image keeps
// whatever it last held, which nothing samples.
class VulkanTransmissionCopyPass : public IScenePass
{
  public:
    VulkanTransmissionCopyPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        const VulkanTransmissionImage& image);
    ~VulkanTransmissionCopyPass() override;

    VulkanTransmissionCopyPass(const VulkanTransmissionCopyPass&) = delete;
    VulkanTransmissionCopyPass& operator=(const VulkanTransmissionCopyPass&) = delete;

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
    const VulkanTransmissionImage& m_image;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    // Per frame slot: that slot's HDR target into the copy's level 0.
    std::vector<VkDescriptorSet> m_sets;
};
}
