#pragma once

#include "../atmosphere.h"
#include "../camera.h"
#include "../light_clusters.h"
#include "../local_shadows.h"
#include "../material.h"
#include "../specular_aa.h"
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
    // Bindings 15 and 16: the area lights' LTC tables (ltc_table.h), in SHADER_READ_ONLY_OPTIMAL.
    TextureDescriptorBinding ltcInverseMatrices;
    TextureDescriptorBinding ltcAmplitudes;
    // Binding 18: the transmission copy (VulkanTransmissionImage), in SHADER_READ_ONLY_OPTIMAL.
    TextureDescriptorBinding transmission;
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
    TextureDescriptorBinding clearcoat;
    TextureDescriptorBinding clearcoatRoughness;
    TextureDescriptorBinding sheenColor;
    TextureDescriptorBinding sheenRoughness;
    TextureDescriptorBinding anisotropy;
    TextureDescriptorBinding specular;
    TextureDescriptorBinding specularColor;
    TextureDescriptorBinding clearcoatNormal;
    TextureDescriptorBinding iridescence;
    TextureDescriptorBinding iridescenceThickness;
    TextureDescriptorBinding transmission;
    TextureDescriptorBinding thickness;
};

// Set 1's combined image samplers, in binding order: the six primary maps, layer B's six, the
// blend mask, then the ten layer maps (material_layers.glsl).
inline constexpr uint32_t kMaterialTextureBindingCount = 25;

// Per-light GPU data, 5 x vec4 = 80 bytes, matching SceneLightData in shaders/vulkan/scene_common.glsl.
// positionAndRange : xyz = world position, w = effective range (metres)
// colorAndIntensity: xyz = linear RGB color, w = intensity (lumens or lux)
// directionAndType : xyz = world direction (normalized), w = LightType enum cast to float
// spotAndArea      : x = cos(innerAngle), y = cos(outerAngle), z = areaWidth (area) or source radius
//                    in metres (point, spot), w = areaHeight
struct GpuLightData
{
    glm::vec4 positionAndRange{0.0f, 0.0f, 0.0f, 10.0f};
    glm::vec4 colorAndIntensity{1.0f, 1.0f, 1.0f, 1000.0f};
    glm::vec4 directionAndType{0.0f, -1.0f, 0.0f, 1.0f}; // type 1 = point
    glm::vec4 spotAndArea{0.97f, 0.87f, 1.0f, 1.0f};
    // xyz = the world axis the area light's width runs along; its height runs along
    // cross(direction, right). Only area lights read it. w = 1 + the light's first tile in the local
    // shadow atlas, 0 when it casts no shadow (see LocalShadowPlan).
    glm::vec4 areaRightAxis{1.0f, 0.0f, 0.0f, 0.0f};
};

// One tile of the local shadow atlas as the shader reads it, set 0 binding 14. Matches
// LocalShadowTileData in shaders/vulkan/pbr_common.glsl under std430.
struct GpuLocalShadowTile
{
    glm::mat4 viewProjection{1.0f};
    // uv offset xy, uv size zw.
    glm::vec4 atlasRect{0.0f};
    // x = LocalShadowTile::texelScale, y = 1 on a cube face; zw unused.
    glm::vec4 params{0.0f};
};

static_assert(sizeof(GpuLocalShadowTile) == 96, "GpuLocalShadowTile must stay mat4 + 2 x vec4");

// The storage buffer at set 0 binding 10 holds this many; SelectSceneLights drops the rest.
static constexpr uint32_t kMaxSceneLights = 1024;

// What Update uploads for the lights. The lights are ordered as SelectSceneLights orders them:
// every directional light first, so the shader can loop over those without the cluster grid, then
// the local lights the grid indexes. clusters is read only when clustered is set.
struct LightUpload
{
    std::span<const GpuLightData> lights;
    uint32_t directionalCount = 0;
    const LightClusterGrid* clusters = nullptr;
    // Off: the shader loops over every local light, the brute-force comparison path.
    bool clustered = true;
    // The local shadow atlas tiles the lights' areaRightAxis.w point into; at most
    // kLocalShadowTileCount.
    std::span<const GpuLocalShadowTile> shadowTiles;
};

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
    // x = directional light count, y = total light count, z = 1 when the shader looks lights up
    // through the cluster grid, 0 when it loops over all of them; w unused. The lights themselves
    // are in the storage buffer at binding 10.
    glm::uvec4 lightCounts{0u, 0u, 0u, 0u};
    // x = sliceScale, y = sliceBias of the cluster grid (see LightClusterGrid); zw unused.
    glm::vec4 lightClusterSlices{0.0f};
    ShadowUniformData shadow;
    // Inverse of proj * view, for reconstructing world position from depth in the lighting pass.
    // Appended last so no earlier member's offset moves.
    glm::mat4 invViewProj{1.0f};
    // Last frame's proj * view, for motion vectors. Equal to this frame's when there is no history
    // (the first frame, or the first after the scene targets were rebuilt).
    glm::mat4 prevViewProj{1.0f};
    // Appended last so no earlier member's offset moves.
    EnvironmentUniformData environment;
    // This frame's proj * view without the TAA jitter, which proj and invViewProj carry. Motion
    // vectors are measured with it, so a still camera has none whatever the jitter. Appended last.
    glm::mat4 viewProjNoJitter{1.0f};
    // Geometric specular anti-aliasing: x = 1 when on, y = variance, z = threshold (see
    // specular_aa.h); w unused. Appended last.
    glm::vec4 specularAntiAliasing{0.0f};
    // x = pre-exposure, physical radiance to HDR target units (see PreExposureFromEv100);
    // y = 1 / pre-exposure; zw unused. Appended last.
    glm::vec4 exposure{1.0f, 1.0f, 0.0f, 0.0f};
};

// This struct is memcpy'd straight into the GPU uniform buffer, so its byte layout must match
// the shader's std140 CameraBuffer block exactly. Every member is a 16-byte multiple (mat4/vec4
// only — never add vec3/scalars without manual padding), which keeps the C++ layout identical
// to std140 without relying on GLM alignment macros.
static_assert(sizeof(GpuLightData) == 80, "GpuLightData must stay 5 x vec4 to match std140 and std430");
// view, proj, cameraWorldPosition, ambientLuminance, lightCounts, lightClusterSlices.
inline constexpr size_t kCameraBlockHeaderBytes = 2 * 64 + 4 * 16;
static_assert(
    sizeof(CameraUniformData) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16 + 64 + 16 + 16,
    "CameraUniformData layout drifted from the shader CameraBuffer std140 block");
static_assert(
    offsetof(CameraUniformData, exposure) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16 + 64 + 16,
    "exposure must follow specularAntiAliasing with no padding");
static_assert(
    offsetof(CameraUniformData, specularAntiAliasing) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16 + 64,
    "specularAntiAliasing must follow viewProjNoJitter with no padding");
static_assert(
    offsetof(CameraUniformData, viewProjNoJitter) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64 + 18 * 16,
    "viewProjNoJitter must follow the environment block with no padding");
static_assert(
    offsetof(CameraUniformData, shadow) == kCameraBlockHeaderBytes,
    "the shadow block must follow lightClusterSlices with no padding");
static_assert(
    offsetof(CameraUniformData, invViewProj) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16,
    "invViewProj must follow the shadow block with no padding, where std140 places it");
static_assert(
    offsetof(CameraUniformData, prevViewProj) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64,
    "prevViewProj must follow invViewProj with no padding");
static_assert(
    offsetof(CameraUniformData, environment) ==
        kCameraBlockHeaderBytes + kShadowCascadeCount * 64 + 3 * 16 + 64 + 64,
    "environment must follow prevViewProj with no padding");

// Set 0: the per-frame camera uniform buffer at binding 0, the directional shadow map at binding
// 1, the scene lights at binding 10, the light cluster grid at binding 11, each draw's material at
// binding 12, the local shadow atlas at binding 13 and its tiles at binding 14, the LTC tables at
// bindings 15 and 16, each draw's texture transforms at binding 17, and each draw's previous model matrix at binding 2 (a storage buffer read by triangle.vert for
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
        TextureDescriptorBinding localShadowAtlas,
        EnvironmentDescriptorBindings environment,
        // One per draw slot, in slot order: binding 12 holds them and binding 2 gets as many
        // previous-model slots, so the two can never disagree about how many draws there are.
        std::span<const GpuMaterialData> drawMaterials,
        // Parallel to drawMaterials: set 0 binding 17.
        std::span<const GpuTextureTransforms> drawTextureTransforms);
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
        const LightUpload& lights,
        const ShadowUniformData& shadow,
        const glm::mat4& prevViewProj,
        std::span<const glm::mat4> prevModels,
        const EnvironmentUniformData& environment,
        const glm::mat4& viewProjNoJitter,
        bool specularAntiAliasing,
        float preExposure);

  private:
    // Shared by the destructor and the constructor's unwind path. Skips null handles.
    void DestroyHandles();
    void CreateBuffers(uint32_t imageCount);
    // A host-visible, host-coherent buffer, mapped for its lifetime. The handles are written as
    // they are created, so DestroyHandles releases whatever a throw part way through left behind.
    void CreateMappedBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        void*& mapped);
    void CreateDescriptorPool(uint32_t imageCount);
    void CreateDescriptorSets(uint32_t imageCount);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<MaterialTextureBinding> m_materialBindings;
    TextureDescriptorBinding m_shadowMap;
    TextureDescriptorBinding m_localShadowAtlas;
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
    // Set 0 binding 12: every draw's material, written once here and never again. Content changes
    // build a new VulkanUniformBuffer, so one buffer serves every frame in flight.
    VkBuffer m_materialBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_materialMemory = VK_NULL_HANDLE;
    void* m_mappedMaterialBuffer = nullptr;
    std::vector<GpuMaterialData> m_drawMaterials;
    // Set 0 binding 17: every draw's texture transforms, like the materials written once.
    VkBuffer m_textureTransformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_textureTransformMemory = VK_NULL_HANDLE;
    void* m_mappedTextureTransformBuffer = nullptr;
    std::vector<GpuTextureTransforms> m_drawTextureTransforms;
    // Set 0 binding 10: every light the shader evaluates, kMaxSceneLights slots per image.
    std::vector<VkBuffer> m_lightBuffers;
    std::vector<VkDeviceMemory> m_lightMemories;
    std::vector<void*> m_mappedLightBuffers;
    // Set 0 binding 11: the cluster ranges, then the index list, one per image.
    std::vector<VkBuffer> m_clusterBuffers;
    std::vector<VkDeviceMemory> m_clusterMemories;
    std::vector<void*> m_mappedClusterBuffers;
    // Set 0 binding 14: the local shadow atlas tiles, kLocalShadowTileCount slots per image.
    std::vector<VkBuffer> m_shadowTileBuffers;
    std::vector<VkDeviceMemory> m_shadowTileMemories;
    std::vector<void*> m_mappedShadowTileBuffers;
    std::vector<VkDescriptorSet> m_frameDescriptorSets;
    std::vector<VkDescriptorSet> m_descriptorSets;
    uint32_t m_imageCount = 0;
};
}
