#pragma once

#include "../atmosphere.h"
#include "../camera.h"
#include "../shadow_cascades.h"
#include "common.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace me
{

struct TextureDescriptorBinding
{
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

// The environment images set 0 binds for the fragment shaders: the atmosphere LUTs, kept in
// VK_IMAGE_LAYOUT_GENERAL by VulkanAtmosphere, and the equirectangular HDRI (a 1x1 black map when
// none is loaded), in SHADER_READ_ONLY_OPTIMAL.
struct EnvironmentDescriptorBindings
{
    TextureDescriptorBinding transmittance;
    TextureDescriptorBinding skyView;
    TextureDescriptorBinding aerialPerspective;
    TextureDescriptorBinding environmentMap;
    // Binding 7: the atmosphere's radiance SH (VulkanAtmosphere::GetIrradianceBuffer).
    VkBuffer irradiance = VK_NULL_HANDLE;
    // Binding 8: the GGX-prefiltered sky cube (VulkanEnvironmentProbe), in GENERAL.
    TextureDescriptorBinding prefiltered;
    // Binding 9: the DFG table, in SHADER_READ_ONLY_OPTIMAL.
    TextureDescriptorBinding brdfLut;
};

struct MaterialTextureBinding
{
    TextureDescriptorBinding baseColor;
    TextureDescriptorBinding normal;
    TextureDescriptorBinding metallic;
    TextureDescriptorBinding roughness;
    TextureDescriptorBinding occlusion;
    TextureDescriptorBinding emissive;
    TextureDescriptorBinding secondaryBaseColor;
    TextureDescriptorBinding secondaryNormal;
    TextureDescriptorBinding secondaryMetallic;
    TextureDescriptorBinding secondaryRoughness;
    TextureDescriptorBinding secondaryOcclusion;
    TextureDescriptorBinding secondaryEmissive;
    TextureDescriptorBinding blendMask;
};

// Per-light GPU data, 5 x vec4 = 80 bytes, matching SceneLightData in shaders/vulkan/scene_common.glsl.
// positionAndRange : xyz = world position, w = effective range (metres)
// colorAndIntensity: xyz = linear RGB color, w = intensity (lumens or lux)
// directionAndType : xyz = world direction (normalized), w = LightType enum cast to float
// spotAndArea      : x = cos(innerAngle), y = cos(outerAngle), z = areaWidth, w = areaHeight
struct GpuLightData
{
    glm::vec4 positionAndRange{0.0f, 0.0f, 0.0f, 10.0f};
    glm::vec4 colorAndIntensity{1.0f, 1.0f, 1.0f, 1000.0f};
    glm::vec4 directionAndType{0.0f, -1.0f, 0.0f, 1.0f}; // type 1 = point
    glm::vec4 spotAndArea{0.97f, 0.87f, 1.0f, 1.0f};
    // xyz = the world axis the area light's width runs along; its height runs along
    // cross(direction, right). Only area lights read it.
    glm::vec4 areaRightAxis{1.0f, 0.0f, 0.0f, 0.0f};
};

static constexpr uint32_t kMaxSceneLights = 8;

// The directional shadow map as the shader reads it. Mirrors the shadow members at the end of
// CameraBuffer in shaders/vulkan/scene_common.glsl.
struct ShadowUniformData
{
    glm::mat4 cascadeViewProjection[kShadowCascadeCount]{};
    // Component i: the view distance where cascade i ends.
    glm::vec4 cascadeSplits{0.0f};
    // Component i: the world size of one texel of cascade i.
    glm::vec4 cascadeTexelSizes{0.0f};
    // x = index into the uploaded lights of the light that casts shadows, or -1 for none;
    // y = 1 / shadow map resolution.
    glm::vec4 params{-1.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(kShadowCascadeCount == 4, "ShadowUniformData packs one cascade per vec4 component");

struct alignas(16) CameraUniformData
{
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec4 cameraWorldPosition{0.0f, 0.0f, 0.0f, 1.0f};
    // xyz = ambient luminance in cd/m^2 (see SceneLightSelection::ambientLuminance), w unused.
    glm::vec4 ambientLuminance{0.0f};
    GpuLightData lights[kMaxSceneLights];
    glm::uvec4 sceneLightCount{0u, 0u, 0u, 0u};
    ShadowUniformData shadow;
    // Inverse of proj * view, for reconstructing world position from depth in the lighting pass.
    // Appended last so no earlier member's offset moves.
    glm::mat4 invViewProj{1.0f};
    // Last frame's proj * view, for motion vectors. Equal to this frame's when there is no history
    // (the first frame, or the first after the scene targets were rebuilt).
    glm::mat4 prevViewProj{1.0f};
    // Appended last so no earlier member's offset moves.
    EnvironmentUniformData environment;
};

// This struct is memcpy'd straight into the GPU uniform buffer, so its byte layout must match
// the shader's std140 CameraBuffer block exactly. Every member is a 16-byte multiple (mat4/vec4
// only — never add vec3/scalars without manual padding), which keeps the C++ layout identical
// to std140 without relying on GLM alignment macros.
static_assert(sizeof(GpuLightData) == 80, "GpuLightData must stay 5 x vec4 to match std140");
static_assert(
    sizeof(CameraUniformData) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16,
    "CameraUniformData layout drifted from the shader CameraBuffer std140 block");
static_assert(
    offsetof(CameraUniformData, invViewProj) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16,
    "invViewProj must follow the shadow block with no padding, where std140 places it");
static_assert(
    offsetof(CameraUniformData, prevViewProj) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16 + 64,
    "prevViewProj must follow invViewProj with no padding");
static_assert(
    offsetof(CameraUniformData, environment) ==
        2 * 64 + 2 * 16 + kMaxSceneLights * 80 + 16 + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64,
    "environment must follow prevViewProj with no padding");

// Set 0: the per-frame camera uniform buffer at binding 0, the directional shadow map at binding
// 1 and each draw's previous model matrix at binding 2 (a storage buffer read by triangle.vert for
// motion vectors). Split out from the material set so that the camera write leaves the
// per-material loop entirely — it is written once per swapchain image instead of once per image
// per material — and so a material reload rebuilds only set 1. The deferred lighting pass binds
// this same set with no material at all; the tone mapping pass binds no camera set.
class VulkanFrameDescriptorSetLayout
{
  public:
    explicit VulkanFrameDescriptorSetLayout(VkDevice device);
    ~VulkanFrameDescriptorSetLayout();

    VulkanFrameDescriptorSetLayout(const VulkanFrameDescriptorSetLayout&) = delete;
    VulkanFrameDescriptorSetLayout& operator=(const VulkanFrameDescriptorSetLayout&) = delete;

    VkDescriptorSetLayout GetHandle() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};

// The material descriptor set layout is fixed by the shader (13 combined image samplers) and
// never varies with scene content or swapchain size. It is owned separately from
// VulkanUniformBuffer so that rebuilding descriptor sets for a new texture set — which happens on
// every model import — does not invalidate the pipelines built against this layout.
class VulkanMaterialDescriptorSetLayout
{
  public:
    explicit VulkanMaterialDescriptorSetLayout(VkDevice device);
    ~VulkanMaterialDescriptorSetLayout();

    VulkanMaterialDescriptorSetLayout(const VulkanMaterialDescriptorSetLayout&) = delete;
    VulkanMaterialDescriptorSetLayout& operator=(const VulkanMaterialDescriptorSetLayout&) = delete;

    VkDescriptorSetLayout GetHandle() const;

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};

class VulkanUniformBuffer
{
  public:
    VulkanUniformBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t imageCount,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout materialSetLayout,
        const std::vector<MaterialTextureBinding>& materialBindings,
        TextureDescriptorBinding shadowMap,
        EnvironmentDescriptorBindings environment,
        uint32_t motionSlotCount);
    ~VulkanUniformBuffer();

    VulkanUniformBuffer(const VulkanUniformBuffer&) = delete;
    VulkanUniformBuffer& operator=(const VulkanUniformBuffer&) = delete;

    VkDescriptorSet GetFrameDescriptorSet(uint32_t imageIndex) const;
    // Points set 0 binding 6 of every frame set at another environment map. The caller has waited
    // for every frame in flight: the sets must not be in use while they are written.
    void SetEnvironmentMap(TextureDescriptorBinding environmentMap);
    VkDescriptorSet GetDescriptorSet(uint32_t imageIndex, uint32_t materialIndex) const;
    void Update(
        uint32_t imageIndex,
        const ViewportMatrices& matrices,
        const glm::vec3& cameraPosition,
        const glm::vec3& ambientLuminance,
        bool usesFallbackAmbient,
        std::span<const GpuLightData> lights,
        const ShadowUniformData& shadow,
        const glm::mat4& prevViewProj,
        std::span<const glm::mat4> prevModels,
        const EnvironmentUniformData& environment);

  private:
    // Shared by the destructor and the constructor's unwind path. Skips null handles.
    void DestroyHandles();
    void CreateBuffers(uint32_t imageCount);
    void CreateDescriptorPool(uint32_t imageCount);
    void CreateDescriptorSets(uint32_t imageCount);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<MaterialTextureBinding> m_materialBindings;
    TextureDescriptorBinding m_shadowMap;
    EnvironmentDescriptorBindings m_environment;
    VkDescriptorSetLayout m_frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkBuffer> m_buffers;
    std::vector<VkDeviceMemory> m_memories;
    std::vector<void*> m_mappedBuffers;
    // Set 0 binding 2: each draw's previous model matrix, indexed by its firstInstance. One
    // host-visible buffer per swapchain image, sized to the draw list this object was built for.
    std::vector<VkBuffer> m_motionBuffers;
    std::vector<VkDeviceMemory> m_motionMemories;
    std::vector<void*> m_mappedMotionBuffers;
    uint32_t m_motionSlotCount = 0;
    std::vector<VkDescriptorSet> m_frameDescriptorSets;
    std::vector<VkDescriptorSet> m_descriptorSets;
    uint32_t m_imageCount = 0;
};
}
