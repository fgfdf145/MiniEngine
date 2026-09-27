#pragma once

#include "common.h"
#include "upload_batch.h"

#include <engine/asset/mesh.h>

#include <array>
#include <cstdint>

namespace me
{

VkVertexInputBindingDescription GetVertexBindingDescription();
std::array<VkVertexInputAttributeDescription, 6> GetVertexAttributeDescriptions();
// The position-only stream (VulkanBuffer::GetPositionHandle): binding 0, location 0, 12 bytes a
// vertex. Depth-only passes read it instead of the full vertex, a fifth of the bytes.
VkVertexInputBindingDescription GetPositionBindingDescription();
VkVertexInputAttributeDescription GetPositionAttributeDescription();

class VulkanBuffer
{
  public:
    // Self-contained: builds a one-shot internal upload batch and flushes it immediately.
    // Use for one-off buffers outside of bulk model loading.
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue);

    // Records this buffer's vertex/index upload into a caller-supplied batch instead of
    // submitting and waiting on its own. The caller must call uploadBatch.Flush() (directly or
    // via destruction) before the buffers are used, and keep the batch alive until then.
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const MeshData& meshData,
        VulkanUploadBatch& uploadBatch);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VkBuffer GetVertexHandle() const;
    VkBuffer GetIndexHandle() const;
    // Each vertex's position alone, tightly packed; indexed by the same index buffer.
    VkBuffer GetPositionHandle() const;
    uint32_t GetVertexCount() const;
    uint32_t GetIndexCount() const;

  private:
    // Shared by the destructor and the constructors' unwind path. Skips null handles.
    void DestroyHandles();
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void UploadVertices(const MeshData& meshData, VulkanUploadBatch& uploadBatch);
    void UploadIndices(const MeshData& meshData, VulkanUploadBatch& uploadBatch);
    void UploadPositions(const MeshData& meshData, VulkanUploadBatch& uploadBatch);
    // Stages size bytes and records their copy into a new device-local buffer.
    void UploadDeviceLocal(
        const void* source,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VulkanUploadBatch& uploadBatch,
        VkBuffer& buffer,
        VkDeviceMemory& memory);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    // Only the counts outlive the upload. The mesh itself is owned by the model cache and
    // staged straight from there, so the GPU buffers don't shadow a second host-side copy.
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexMemory = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
    VkBuffer m_positionBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_positionMemory = VK_NULL_HANDLE;
};
}
