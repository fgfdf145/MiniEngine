#pragma once

#include "common.h"
#include "gpu_timer.h"
#include "uniform_buffer.h"

#include <engine/renderer/atmosphere.h>

#include <array>
#include <optional>
#include <vector>

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
    // frameSlot picks the host-visible copy of the sky's SH this frame leaves for the CPU (see
    // GetSkyAverageRadiance).
    // cloudLife is the plumes' phases of life the frame's uniforms carry (EnvironmentUniformData::
    // cloudLife), or null with the clouds off: the plume map is rebuilt whenever they move on.
    void Record(
        VkCommandBuffer commandBuffer,
        VkDescriptorSet frameDescriptorSet,
        const AtmosphereParameters* parameters,
        uint32_t frameSlot,
        const glm::vec4* cloudLife = nullptr);

    // The sky's average radiance (its SH's L0 band), as the frame last recorded in this slot left
    // it, for auto exposure and white balance. Call after the slot's fence has signaled; empty when
    // that frame did not render the atmosphere.
    std::optional<glm::vec3> GetSkyAverageRadiance(uint32_t frameSlot) const;

    TextureDescriptorBinding GetTransmittanceBinding() const;
    TextureDescriptorBinding GetSkyViewBinding() const;
    TextureDescriptorBinding GetAerialPerspectiveBinding() const;
    // The volumetric clouds' noise (cloud_noise.comp), built on the first Record; REPEAT sampler.
    TextureDescriptorBinding GetCloudShapeNoiseBinding() const;
    TextureDescriptorBinding GetCloudDetailNoiseBinding() const;
    // The clouds' shadow map (cloud_shadow.comp), written every frame the atmosphere renders.
    TextureDescriptorBinding GetCloudShadowBinding() const;
    TextureDescriptorBinding GetCloudWeatherBinding() const;

    // The clouds for the sky pass to composite, at the scene's extent
    // (docs/design/2026-10-06-cumulus-generation-design.md, temporal reconstruction): each frame
    // cloud_march.comp marches one pixel of every 2 x 2 block, a quarter of them, and
    // cloud_resolve.comp rebuilds the rest from the reprojected history. EnsureCloudTarget
    // recreates the targets when the extent changed and returns true; the caller has waited for
    // the device and then repoints set 0 binding 28 (VulkanUniformBuffer::SetCloudTarget).
    // RecordClouds marches and resolves, after Record, and orders the sky pass's reads after its
    // writes.
    bool EnsureCloudTarget(VkExtent2D sceneExtent);
    // timer, when given, marks "CloudMarch" between the two passes.
    void RecordClouds(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, VulkanGpuTimer* timer = nullptr);
    TextureDescriptorBinding GetCloudTargetBinding() const;
    VkBuffer GetIrradianceBuffer() const;

  private:
    enum Lut : size_t
    {
        kTransmittance,
        kMultiScattering,
        kSkyView,
        kAerialPerspective,
        kLutCount
    };
    // The pipelines are the four LUTs', the sky's SH projection and the clouds' noise.
    static constexpr size_t kIrradiancePipeline = kLutCount;
    static constexpr size_t kCloudNoisePipeline = kLutCount + 1;
    static constexpr size_t kCloudShadowPipeline = kLutCount + 2;
    static constexpr size_t kCloudWeatherPipeline = kLutCount + 3;
    static constexpr size_t kCloudMarchPipeline = kLutCount + 4;
    static constexpr size_t kCloudResolvePipeline = kLutCount + 5;
    static constexpr size_t kPipelineCount = kLutCount + 6;
    // The clouds' images built once: the two billow volumes and the plume map.
    enum CloudNoise : size_t
    {
        kCloudShape,
        kCloudDetail,
        kCloudWeather,
        kCloudNoiseCount
    };
    // The images the first Record moves out of UNDEFINED: the LUTs, the noise and the shadow map.
    static constexpr size_t kNoiseImagesBegin = kLutCount;
    static constexpr size_t kShadowImageIndex = kNoiseImagesBegin + static_cast<size_t>(kCloudNoiseCount);
    static constexpr size_t kInitialImageCount = kShadowImageIndex + 1;

    struct LutImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void CreateImages();
    void CreateDescriptors();
    void CreateCloudTargets(VkExtent2D sceneExtent);
    void CreateCloudImage(LutImage& image, VkExtent2D extent, VkImageUsageFlags usage, const char* name);
    void DestroyCloudTargets();
    void WriteCloudTargetDescriptors();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void Dispatch(VkCommandBuffer commandBuffer, size_t pipeline, uint32_t x, uint32_t y, uint32_t z) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();
    void DestroyPlumeStaging();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<LutImage, kLutCount> m_images{};
    // RGBA8 volumes, written once and then only sampled; GENERAL like the LUTs.
    std::array<LutImage, kCloudNoiseCount> m_cloudNoise{};
    VkSampler m_cloudSampler = VK_NULL_HANDLE;
    // RGBA16F, r the transmittance toward the sun; GENERAL like the LUTs.
    LutImage m_cloudShadow{};
    // RGBA16F, GENERAL: the march's samples (half the scene's extent, rounded up), the resolved
    // clouds the sky reads and last frame's copy of them, the history (both at the scene's
    // extent). Fresh after (re)creation until their first transition, when there is no history.
    LutImage m_cloudTarget{};
    LutImage m_cloudResolved{};
    LutImage m_cloudHistory{};
    VkExtent2D m_cloudSceneExtent{};
    bool m_cloudTargetFresh = true;
    // Steps the marched pixel through each 2 x 2 block, every frame, with TAA or without.
    uint32_t m_cloudFrame = 0;
    bool m_cloudNoiseBuilt = false;
    // The phases of life the plume map was last built at.
    glm::vec4 m_cloudWeatherLife{0.0f};
    std::array<VkPipeline, kPipelineCount> m_pipelines{};
    // The sky's radiance SH, nine vec4 written by atmosphere_irradiance.comp and read through set 0
    // binding 7. Shared by the frames in flight like the LUTs.
    VkBuffer m_irradianceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_irradianceMemory = VK_NULL_HANDLE;
    // The clouds' plume table (BuildCloudPlumeTable), device local, read by cloud_weather.comp
    // through set 1 binding 14; filled from the host-visible staging copy by the first Record,
    // which is freed once that frame has surely finished.
    VkBuffer m_plumeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_plumeMemory = VK_NULL_HANDLE;
    VkBuffer m_plumeStaging = VK_NULL_HANDLE;
    VkDeviceMemory m_plumeStagingMemory = VK_NULL_HANDLE;
    uint32_t m_plumeStagingAge = 0;
    // One host-visible copy of the SH per frame in flight, copied after the projection so the CPU
    // reads a finished frame's sky instead of racing the shared buffer.
    struct Readback
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const float* mapped = nullptr;
        bool written = false;
    };
    std::vector<Readback> m_readbacks;
    bool m_imagesInitialized = false;
    // The parameters the static LUTs were last built from; empty until the first build.
    std::optional<AtmosphereParameters> m_staticLutParameters;
};
}
