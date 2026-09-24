#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <engine/renderer/material.h>
#include <engine/renderer/shadow_cascades.h>

#include <array>
#include <span>

namespace me
{

// What the shadow pass needs to draw one caster. Blend materials are not casters.
struct ShadowDrawItem
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    glm::mat4 model{1.0f};
    // A world space sphere around the caster; each cascade skips casters that miss its volume.
    glm::vec3 worldBoundsCenter{0.0f};
    float worldBoundsRadius = 0.0f;
    // Mask materials run the alpha test and need their material set; opaque ones need neither.
    bool alphaMask = false;
    VkDescriptorSet materialDescriptorSet = VK_NULL_HANDLE;
    GpuMaterialData material;
};

// Push constants for shaders/vulkan/shadow.vert and shadow.frag.
struct ShadowPushConstants
{
    glm::mat4 lightModelViewProjection{1.0f};
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float nodeGraphFactors[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    float alphaCutoffAndPadding[4] = {0.5f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(ShadowPushConstants) == 112, "ShadowPushConstants must match the shadow shaders' block");

// Renders the cascaded shadow map of the directional light that casts shadows: a 2D array depth
// image with one layer per cascade, which the material pass samples through set 0, binding 1.
//
// This pass is not an IScenePass, and its image is not a SceneRenderTargets target. It has a fixed
// resolution rather than the viewport's, it has one layer per cascade, and it is one image shared
// by every frame in flight rather than a copy per frame. So it manages its own layouts: each
// cascade's render pass clears the layer from UNDEFINED and leaves it SHADER_READ_ONLY_OPTIMAL,
// and the render pass's external dependencies order it against the previous frame's reads and
// this frame's material pass. RenderTargetLayoutTracker never sees this image.
//
// The pass lives as long as the device: nothing in it depends on the swapchain or the viewport.
class VulkanShadowPass
{
  public:
    VulkanShadowPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout materialSetLayout,
        uint32_t resolution);
    ~VulkanShadowPass();

    VulkanShadowPass(const VulkanShadowPass&) = delete;
    VulkanShadowPass& operator=(const VulkanShadowPass&) = delete;

    uint32_t GetResolution() const;

    // The array view and comparison sampler the material pass binds.
    TextureDescriptorBinding GetSampledBinding() const;

    // Renders every caster into every cascade. With no cascades it only clears the layers, which
    // still has to happen every frame: the material pass binds the map whether or not a light
    // casts shadows, and the clear is what puts each layer in the layout that binding declares.
    void Record(
        VkCommandBuffer commandBuffer,
        std::span<const ShadowDrawItem> drawItems,
        const ShadowCascades* cascades) const;

  private:
    void CreateImage(VkPhysicalDevice physicalDevice);
    void CreateSampler(VkPhysicalDevice physicalDevice);
    void CreateRenderPass();
    void CreateFramebuffers();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout materialSetLayout);
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_resolution = 0;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_arrayView = VK_NULL_HANDLE;
    std::array<VkImageView, kShadowCascadeCount> m_layerViews{};
    std::array<VkFramebuffer, kShadowCascadeCount> m_framebuffers{};
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_opaquePipeline = VK_NULL_HANDLE;
    VkPipeline m_maskPipeline = VK_NULL_HANDLE;
};
}
