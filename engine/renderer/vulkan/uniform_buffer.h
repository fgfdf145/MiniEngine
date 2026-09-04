#pragma once

#include "../camera.h"
#include "common.h"

#include <glm/glm.hpp>

#include <vector>

namespace me
{

struct TextureDescriptorBinding
{
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
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

// Per-light GPU data — 4 × vec4 = 64 bytes, matches GLSL std140 struct layout.
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
};

static constexpr uint32_t kMaxSceneLights = 8;

struct alignas(16) CameraUniformData
{
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec4 cameraWorldPosition{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 ambientColorAndIntensity{0.05f, 0.05f, 0.08f, 1.0f};
    GpuLightData lights[kMaxSceneLights];
    glm::uvec4 sceneLightCount{0u, 0u, 0u, 0u};
};

// This struct is memcpy'd straight into the GPU uniform buffer, so its byte layout must match
// the shader's std140 CameraBuffer block exactly. Every member is a 16-byte multiple (mat4/vec4
// only — never add vec3/scalars without manual padding), which keeps the C++ layout identical
// to std140 without relying on GLM alignment macros.
static_assert(sizeof(GpuLightData) == 64, "GpuLightData must stay 4 x vec4 to match std140");
static_assert(
    sizeof(CameraUniformData) == 2 * 64 + 2 * 16 + kMaxSceneLights * 64 + 16,
    "CameraUniformData layout drifted from the shader CameraBuffer std140 block");

// The material descriptor set layout is fixed by the shader (one uniform buffer plus 13 combined
// image samplers) and never varies with scene content or swapchain size. It is owned separately
// from VulkanUniformBuffer so that rebuilding descriptor sets for a new texture set — which
// happens on every model import — does not invalidate the pipelines built against this layout.
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
        VkDescriptorSetLayout descriptorSetLayout,
        const std::vector<MaterialTextureBinding>& materialBindings);
    ~VulkanUniformBuffer();

    VulkanUniformBuffer(const VulkanUniformBuffer&) = delete;
    VulkanUniformBuffer& operator=(const VulkanUniformBuffer&) = delete;

    VkDescriptorSet GetDescriptorSet(uint32_t imageIndex, uint32_t materialIndex) const;
    void Update(
        uint32_t imageIndex,
        const ViewportMatrices& matrices,
        const glm::vec3& cameraPosition,
        const std::vector<GpuLightData>& lights);

  private:
    void CreateBuffers(uint32_t imageCount);
    void CreateDescriptorPool(uint32_t imageCount);
    void CreateDescriptorSets(uint32_t imageCount);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<MaterialTextureBinding> m_materialBindings;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkBuffer> m_buffers;
    std::vector<VkDeviceMemory> m_memories;
    std::vector<void*> m_mappedBuffers;
    std::vector<VkDescriptorSet> m_descriptorSets;
    uint32_t m_imageCount = 0;
};
}
