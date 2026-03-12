#pragma once

#include "common.h"

#include <array>
#include <vector>

struct Vertex
{
    float position[3];
    float color[3];
    float texCoord[2];
    float normal[3];
    float tangent[4];

    static VkVertexInputBindingDescription GetBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 5> GetAttributeDescriptions();
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    bool IsValid() const
    {
        return !vertices.empty() && !indices.empty();
    }
};

class VulkanBuffer
{
public:
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue
    );
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue,
        const MeshData& meshData
    );
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VkBuffer GetVertexHandle() const;
    VkBuffer GetIndexHandle() const;
    uint32_t GetVertexCount() const;
    uint32_t GetIndexCount() const;

private:
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
    void CopyBuffer(VkBuffer sourceBuffer, VkBuffer destinationBuffer, VkDeviceSize size) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void UploadVertices();
    void UploadIndices();
    static MeshData CreateDefaultCubeMesh();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
};
