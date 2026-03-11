#pragma once

#include "camera.h"
#include "common.h"

#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#include <glm/glm.hpp>

struct alignas(16) CameraUniformData
{
    glm::mat4 model{ 1.0f };
    glm::mat4 view{ 1.0f };
    glm::mat4 proj{ 1.0f };
};

class VulkanUniformBuffer
{
public:
    VulkanUniformBuffer(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t imageCount);
    ~VulkanUniformBuffer();

    VulkanUniformBuffer(const VulkanUniformBuffer&) = delete;
    VulkanUniformBuffer& operator=(const VulkanUniformBuffer&) = delete;

    VkDescriptorSetLayout GetDescriptorSetLayout() const;
    VkDescriptorSet GetDescriptorSet(uint32_t imageIndex) const;
    void Update(uint32_t imageIndex, const Camera& camera, VkExtent2D extent);

private:
    void CreateDescriptorSetLayout();
    void CreateBuffers(uint32_t imageCount);
    void CreateDescriptorPool(uint32_t imageCount);
    void CreateDescriptorSets(uint32_t imageCount);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkBuffer> m_buffers;
    std::vector<VkDeviceMemory> m_memories;
    std::vector<void*> m_mappedBuffers;
    std::vector<VkDescriptorSet> m_descriptorSets;
};
