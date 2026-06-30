#pragma once

#include "common.h"
#include "upload_batch.h"

#include <mesh.h>

#include <array>
#include <vector>

VkVertexInputBindingDescription GetVertexBindingDescription();
std::array<VkVertexInputAttributeDescription, 5> GetVertexAttributeDescriptions();

class VulkanBuffer
{
public:
    // Self-contained: builds a one-shot internal upload batch and flushes it immediately.
    // Use for one-off buffers outside of bulk model loading.
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue
    );

    // Records this buffer's vertex/index upload into a caller-supplied batch instead of
    // submitting and waiting on its own. The caller must call uploadBatch.Flush() (directly or
    // via destruction) before the buffers are used, and keep the batch alive until then.
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const MeshData& meshData,
        VulkanUploadBatch& uploadBatch
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
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void UploadVertices(VulkanUploadBatch& uploadBatch);
    void UploadIndices(VulkanUploadBatch& uploadBatch);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
};
