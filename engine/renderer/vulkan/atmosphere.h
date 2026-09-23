#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <engine/renderer/atmosphere.h>

#include <array>
#include <optional>

namespace me
{

// Hillaire 2020's four LUTs: transmittance and multiple scattering (rebuilt when the atmosphere's
// parameters change), sky-view and the aerial perspective volume (every frame). Like the shadow
// map, this is not an IScenePass and its images are not render targets: they have fixed sizes and
// one copy shared by every frame in flight, so they stay in VK_IMAGE_LAYOUT_GENERAL and Record
// orders itself with its own barriers. A barrier's first scope is every command submitted earlier
// on the queue, so the one at the head of Record covers the previous frame's fragment reads.
//
// Set 0 names these images for every draw, whatever the mode, so the first Record moves them out
// of UNDEFINED and clears them even when the atmosphere is off.
class VulkanAtmosphere
{
  public:
    VulkanAtmosphere(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanAtmosphere();

    VulkanAtmosphere(const VulkanAtmosphere&) = delete;
    VulkanAtmosphere& operator=(const VulkanAtmosphere&) = delete;

    // parameters is null when the frame does not render the atmosphere; the frame descriptor set
    // carries the same parameters in its camera block.
    void Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, const AtmosphereParameters* parameters);

    TextureDescriptorBinding GetTransmittanceBinding() const;
    TextureDescriptorBinding GetSkyViewBinding() const;
    TextureDescriptorBinding GetAerialPerspectiveBinding() const;

  private:
    enum Lut : size_t
    {
        kTransmittance,
        kMultiScattering,
        kSkyView,
        kAerialPerspective,
        kLutCount
    };

    struct LutImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void CreateImages();
    void CreateDescriptors();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void Dispatch(VkCommandBuffer commandBuffer, Lut lut, uint32_t x, uint32_t y, uint32_t z) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<LutImage, kLutCount> m_images{};
    std::array<VkPipeline, kLutCount> m_pipelines{};
    bool m_imagesInitialized = false;
    // The parameters the static LUTs were last built from; empty until the first build.
    std::optional<AtmosphereParameters> m_staticLutParameters;
};
}
