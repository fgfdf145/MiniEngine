#pragma once

#include "common.h"
#include "shadow_pass.h"
#include "uniform_buffer.h"

#include <engine/renderer/local_shadows.h>

#include <span>

namespace me
{

// Renders the local lights' shadow atlas (see local_shadows.h): one 2D depth image, one viewport
// per tile, which the material pass samples through set 0, binding 13. The casters and their push
// constants are the cascade pass's, and so are the shaders.
//
// Like VulkanShadowPass it is not an IScenePass: the atlas has a fixed size, is one image shared by
// every frame in flight, and orders itself through its render pass dependencies. Its render pass
// clears the whole atlas from UNDEFINED and leaves it SHADER_READ_ONLY_OPTIMAL, every frame, tiles
// or none, because the material pass binds it either way. RenderTargetLayoutTracker never sees it.
class VulkanLocalShadowPass
{
  public:
    VulkanLocalShadowPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout materialSetLayout);
    ~VulkanLocalShadowPass();

    VulkanLocalShadowPass(const VulkanLocalShadowPass&) = delete;
    VulkanLocalShadowPass& operator=(const VulkanLocalShadowPass&) = delete;

    // The atlas view and comparison sampler the material pass binds.
    TextureDescriptorBinding GetSampledBinding() const;

    // Clears the atlas and renders into each tile every caster whose bounds reach its frustum.
    void Record(
        VkCommandBuffer commandBuffer,
        std::span<const ShadowDrawItem> drawItems,
        std::span<const LocalShadowTile> tiles) const;

  private:
    void CreateImage(VkPhysicalDevice physicalDevice);
    void CreateSampler(VkPhysicalDevice physicalDevice);
    void CreateRenderPass();
    void CreateFramebuffer();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout materialSetLayout);
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_opaquePipeline = VK_NULL_HANDLE;
    VkPipeline m_maskPipeline = VK_NULL_HANDLE;
};
}
