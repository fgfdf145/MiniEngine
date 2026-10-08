#pragma once

#include "common.h"
#include "gpu_timer.h"
#include "nvrhi_native.h"
#include "uniform_buffer.h"

#include <engine/renderer/atmosphere.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace me
{

// Hillaire 2020's four LUTs: transmittance and multiple scattering (rebuilt when the atmosphere's
// parameters change), sky-view and the aerial perspective volume (every frame). Like the shadow
// map, this is not an IScenePass and its images are not render targets: they have fixed sizes and
// one copy shared by every frame in flight, so Record orders itself with its own barriers. A
// barrier's first scope is every command submitted earlier on the queue, so the one at the head of
// Record covers the previous frame's fragment reads. What set 0 samples (the transmittance and
// sky-view LUTs, the aerial perspective volume, the cloud noise, shadow map and resolved clouds)
// rests in SHADER_READ_ONLY_OPTIMAL, the layout an NVRHI binding set names it in, and is GENERAL
// only around its writes (BeginFrameImageWrites); the rest stays GENERAL.
//
// Set 0 names these images for every draw, whatever the mode, so the first Record moves them out
// of UNDEFINED and clears them even when the atmosphere is off (and a view's first RecordView its
// own).
class VulkanAtmosphere
{
  public:
    VulkanAtmosphere(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanAtmosphere();

    VulkanAtmosphere(const VulkanAtmosphere&) = delete;
    VulkanAtmosphere& operator=(const VulkanAtmosphere&) = delete;

    // The shared part of the frame: the LUTs but the aerial perspective, the clouds' noise, plume
    // map and shadow map, and the sky's SH. Each view's own part follows in RecordView.
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
    // The multiple-scattering LUT, in GENERAL: the path tracer integrates the air along its rays with it.
    TextureDescriptorBinding GetMultiScatteringBinding() const;
    // The volumetric clouds' noise (cloud_noise.comp), built on the first Record; REPEAT sampler.
    TextureDescriptorBinding GetCloudShapeNoiseBinding() const;
    TextureDescriptorBinding GetCloudDetailNoiseBinding() const;
    // The clouds' shadow map (cloud_shadow.comp), written every frame the atmosphere renders.
    TextureDescriptorBinding GetCloudShadowBinding() const;
    TextureDescriptorBinding GetCloudWeatherBinding() const;
    nvrhi::IBuffer* GetIrradianceBuffer() const;

  private:
    struct LutImage
    {
        VkImage image = VK_NULL_HANDLE;
        nvrhi::TextureHandle texture;
        VkImageView view = VK_NULL_HANDLE;
    };

  public:
    // What one camera the scene is drawn from has of the atmosphere for itself
    // (docs/design/2026-10-07-quad-vehicle-recording-design.md): the aerial perspective volume, which
    // spans that camera's frustum, and its clouds' march target, resolved image and history, with
    // the set 1 that names them. The LUTs, the noise and the cloud shadow map are the atmosphere's,
    // shared by every view. Made by CreateView; outlived by the atmosphere that made it.
    class View
    {
      public:
        ~View();
        View(const View&) = delete;
        View& operator=(const View&) = delete;

      private:
        friend class VulkanAtmosphere;
        explicit View(VkDevice device);

        VkDevice m_device = VK_NULL_HANDLE;
        LutImage m_aerialPerspective{};
        // RGBA16F: the march's samples (half the scene's extent, rounded up), the resolved clouds
        // the sky reads (SHADER_READ_ONLY_OPTIMAL between frames) and last frame's copy of them,
        // the history (both at the scene's extent). Fresh after (re)creation until their first transition, when there is no history.
        LutImage m_cloudTarget{};
        LutImage m_cloudResolved{};
        LutImage m_cloudHistory{};
        VkExtent2D m_cloudSceneExtent{};
        bool m_cloudTargetFresh = true;
        // The aerial perspective volume is moved out of UNDEFINED and cleared by the view's first
        // RecordView (or, for the atmosphere's placeholder view, its first Record).
        bool m_initialized = false;
        // Steps the marched pixel through each 2 x 2 block, every frame, with TAA or without.
        uint32_t m_cloudFrame = 0;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    };

    // A view with placeholder clouds until EnsureCloudTarget names its extent.
    std::unique_ptr<View> CreateView();
    TextureDescriptorBinding GetAerialPerspectiveBinding(const View& view) const;
    // The clouds for the sky pass to composite, at the scene's extent
    // (docs/design/2026-10-06-cumulus-generation-design.md, temporal reconstruction): each frame
    // cloud_march.comp marches one pixel of every 2 x 2 block, a quarter of them, and
    // cloud_resolve.comp rebuilds the rest from the reprojected history. EnsureCloudTarget
    // recreates the view's targets when the extent changed and returns true; the caller has waited
    // for the device and then repoints set 0 binding 28 (VulkanUniformBuffer::SetCloudTarget).
    bool EnsureCloudTarget(View& view, VkExtent2D sceneExtent);
    TextureDescriptorBinding GetCloudTargetBinding(const View& view) const;
    // After Record, with the view's frame set: its aerial perspective volume (when the frame renders
    // the atmosphere), then its clouds marched and resolved, ordered before the sky pass's reads.
    // timer, when given, marks "CloudMarch" between the two cloud passes.
    void RecordView(
        VkCommandBuffer commandBuffer,
        View& view,
        VkDescriptorSet frameDescriptorSet,
        bool atmosphere,
        VulkanGpuTimer* timer = nullptr);

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
    void CreateImages();
    void CreateLutImage(LutImage& image, VkExtent3D extent);
    static void DestroyImage(VkDevice device, LutImage& image);
    void CreateDescriptors();
    void CreateCloudTargets(View& view, VkExtent2D sceneExtent);
    void CreateCloudImage(LutImage& image, VkExtent2D extent, VkImageUsageFlags usage, const char* name);
    // Writes every binding of a view's set 1: the atmosphere's images and buffers, and the view's own.
    void WriteViewDescriptors(const View& view);
    // Moves the view's images out of UNDEFINED and clears its aerial perspective volume.
    void InitializeView(VkCommandBuffer commandBuffer, View& view);
    void RecordClouds(VkCommandBuffer commandBuffer, View& view, VkDescriptorSet frameDescriptorSet, VulkanGpuTimer* timer);
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void Dispatch(VkCommandBuffer commandBuffer, size_t pipeline, uint32_t x, uint32_t y, uint32_t z) const;
    void DestroyHandles();
    void DestroyPlumeStaging();
    // The images shared by every view that set 0 samples.
    std::array<VkImage, 6> FrameSampledImages() const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_sampler;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    // The view whose set the shared passes in Record bind: its aerial perspective volume and clouds
    // are 1 x 1 placeholders that nothing reads.
    std::unique_ptr<View> m_placeholderView;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    // The transmittance, multiple scattering and sky-view LUTs; the aerial perspective volume is each
    // view's own, so its slot stays empty.
    std::array<LutImage, kLutCount> m_images{};
    // RGBA8 volumes, written once and then only sampled.
    std::array<LutImage, kCloudNoiseCount> m_cloudNoise{};
    nvrhi::SamplerHandle m_cloudSampler;
    // RGBA16F, r the transmittance toward the sun.
    LutImage m_cloudShadow{};
    bool m_cloudNoiseBuilt = false;
    // The phases of life the plume map was last built at.
    glm::vec4 m_cloudWeatherLife{0.0f};
    std::array<VkPipeline, kPipelineCount> m_pipelines{};
    // The sky's radiance SH, nine vec4 written by atmosphere_irradiance.comp and read through set 0
    // binding 7. Shared by the frames in flight like the LUTs.
    VkBuffer m_irradianceBuffer = VK_NULL_HANDLE;
    nvrhi::BufferHandle m_irradianceHandle;
    // The clouds' plume table (BuildCloudPlumeTable), device local, read by cloud_weather.comp
    // through set 1 binding 14; filled from the host-visible staging copy by the first Record,
    // which is freed once that frame has surely finished.
    VkBuffer m_plumeBuffer = VK_NULL_HANDLE;
    nvrhi::BufferHandle m_plumeHandle;
    VkBuffer m_plumeStaging = VK_NULL_HANDLE;
    nvrhi::BufferHandle m_plumeStagingHandle;
    uint32_t m_plumeStagingAge = 0;
    // One host-visible copy of the SH per frame in flight, copied after the projection so the CPU
    // reads a finished frame's sky instead of racing the shared buffer.
    struct Readback
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        nvrhi::BufferHandle handle;
        const float* mapped = nullptr;
        bool written = false;
    };
    std::vector<Readback> m_readbacks;
    bool m_imagesInitialized = false;
    // The parameters the static LUTs were last built from; empty until the first build.
    std::optional<AtmosphereParameters> m_staticLutParameters;
};
}
