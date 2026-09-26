#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <array>

namespace me
{

// The sky as the specular lobe sees it: under a physical sky, the sky's radiance is captured into a
// 256 x 256 cube (the radiance cube, the Khronos Sample Viewer's size: 128 left roughness-0
// reflections visibly softer than the viewer's), its mip chain blitted, and a second cube
// prefiltered with GGX for six roughnesses (mip m for roughness m / 5). Both are recaptured only
// when what they show can have changed: the environment's parameters (EnvironmentUniformData, the
// camera's altitude standing in for its position under the atmosphere, which sees no horizontal
// move) or, through Invalidate, the HDRI image; a static sky costs nothing per frame. Like VulkanAtmosphere it is
// device-lifetime, shared by the frames in flight, kept in VK_IMAGE_LAYOUT_GENERAL, and orders
// itself with its own barriers; it records after VulkanAtmosphere, whose sky-view LUT the capture
// samples. Set 0 binding 8 names the prefiltered cube for every draw, so the first Record clears
// both cubes whatever the mode.
class VulkanEnvironmentProbe
{
  public:
    VulkanEnvironmentProbe(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanEnvironmentProbe();

    VulkanEnvironmentProbe(const VulkanEnvironmentProbe&) = delete;
    VulkanEnvironmentProbe& operator=(const VulkanEnvironmentProbe&) = delete;

    // physicalSky is false in EnvironmentMode::None, when nothing is captured. environment is what
    // this frame's uniform block holds; an unchanged one reuses the last capture.
    void Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, bool physicalSky, const EnvironmentUniformData& environment);

    // The next Record recaptures whatever the parameters: the HDRI image behind binding 7 changed.
    void Invalidate();

    TextureDescriptorBinding GetPrefilteredBinding() const;

  private:
    static constexpr uint32_t kCubeSize = 256;
    static constexpr uint32_t kRadianceMipCount = 9;
    static constexpr uint32_t kPrefilterMipCount = 6;

    struct CubeImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView cubeView = VK_NULL_HANDLE;
    };

    CubeImage CreateCube(uint32_t mipCount, VkImageUsageFlags usage) const;
    VkImageView CreateArrayView(VkImage image, uint32_t mip) const;
    void CreateDescriptors();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void RecordMipChain(VkCommandBuffer commandBuffer) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    CubeImage m_radiance;
    CubeImage m_prefiltered;
    VkImageView m_radianceStorageView = VK_NULL_HANDLE;
    std::array<VkImageView, kPrefilterMipCount> m_prefilteredStorageViews{};
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // One per prefiltered mip; set 0 also serves the capture.
    std::array<VkDescriptorSet, kPrefilterMipCount> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_capturePipeline = VK_NULL_HANDLE;
    VkPipeline m_prefilterPipeline = VK_NULL_HANDLE;
    bool m_imagesInitialized = false;
    // What the cubes hold, to skip a capture that would reproduce it. m_captured is false until the
    // first capture, after Invalidate, and whenever the mode is None.
    bool m_captured = false;
    EnvironmentUniformData m_capturedEnvironment{};
};
}
