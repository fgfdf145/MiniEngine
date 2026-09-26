#pragma once

#include "scene_pass.h"
#include "uniform_buffer.h"

namespace me
{

// The scatter pre-pass of KHR_materials_volume_scatter, as the Khronos glTF Sample Viewer draws it
// (scatter.frag): the materials that scatter, alone, into their own RGBA16F colour and D32 depth at
// the scene's extent. triangle.frag (under kScatterPrepass) writes the diffuse light entering the
// surface, pre-exposed, with the draw slot + 1 in alpha; the forward pass then diffuses it through
// the screen (volume_scatter_common.glsl), sampling both images through set 0 bindings 19 and 20.
//
// The images live here rather than in SceneRenderTargets: the frame set binds one image, not one per
// frame slot, and the forward pipelines read them outside the layout tracker. They rest in
// SHADER_READ_ONLY_OPTIMAL; the render pass takes them from UNDEFINED (their contents are redrawn)
// and back, its two external dependencies ordering it after the last frame's reads and before this
// frame's. On a frame without scatter items the pass draws nothing and the images keep what they
// held, which nothing samples. A resize recreates them; the renderer then points set 0 at the new
// ones (VulkanUniformBuffer::SetScatterImages).
class VulkanScatterPass : public IScenePass
{
  public:
    static constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    VulkanScatterPass(VkPhysicalDevice physicalDevice, VkDevice device, const SceneRenderTargets& targets);
    ~VulkanScatterPass() override;

    VulkanScatterPass(const VulkanScatterPass&) = delete;
    VulkanScatterPass& operator=(const VulkanScatterPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The scatter pipelines are built against this; it depends only on the two formats.
    VkRenderPass GetRenderPass() const;
    // What set 0 bindings 19 and 20 sample: nearest, clamped, in SHADER_READ_ONLY_OPTIMAL.
    TextureDescriptorBinding GetLightBinding() const;
    TextureDescriptorBinding GetDepthBinding() const;

  private:
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void CreateRenderPass();
    void CreateImages(VkExtent2D extent);
    void CreateImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkExtent2D extent, Image& image) const;
    void DestroyImages();
    // Moves freshly created images from UNDEFINED to their resting layout, once: the frame set names
    // that layout from the first frame on, whether or not anything scatters.
    void RecordInitialTransition(VkCommandBuffer commandBuffer) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    Image m_light;
    Image m_depth;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    mutable bool m_initialized = false;
};
}
