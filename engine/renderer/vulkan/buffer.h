#pragma once

#include "common.h"
#include "memory_pool.h"
#include "upload_batch.h"

#include <engine/asset/mesh.h>

#include <array>
#include <cstdint>

namespace me
{

VkVertexInputBindingDescription GetVertexBindingDescription();
std::array<VkVertexInputAttributeDescription, 6> GetVertexAttributeDescriptions();
// What toon.vert reads: position, UV 0, normal and UV 1 at their usual locations, the toon outline's
// smoothed normal (Vertex::outlineNormal) at location 6, which nothing else reads, and last frame's
// position from binding 1 (GetPreviousPositionAttributeDescription).
std::array<VkVertexInputAttributeDescription, 6> GetToonVertexAttributeDescriptions();
// The previous frame's positions (VulkanBuffer::GetPreviousPositionHandle) beside the vertex for the
// material pipelines: binding 1, location 7, 12 bytes a vertex, which triangle.vert's motion vectors
// start from.
VkVertexInputBindingDescription GetPreviousPositionBindingDescription();
VkVertexInputAttributeDescription GetPreviousPositionAttributeDescription();
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
    // deviceAddressable (needs bufferDeviceAddress): the vertex and index buffers are storage buffers
    // with device addresses too, which hardware ray tracing's hit shading reads the hit's vertices
    // through (GetVertexAddress, GetIndexAddress).
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const MeshData& meshData,
        VulkanUploadBatch& uploadBatch,
        bool deviceAddressable = false);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VkBuffer GetVertexHandle() const;
    VkBuffer GetIndexHandle() const;
    // Each vertex's position alone, tightly packed; indexed by the same index buffer.
    VkBuffer GetPositionHandle() const;
    uint32_t GetVertexCount() const;
    uint32_t GetIndexCount() const;
    // 0 unless made deviceAddressable.
    VkDeviceAddress GetVertexAddress() const;
    VkDeviceAddress GetIndexAddress() const;
    // A skinned mesh (MeshData::skin): the vertex and position buffers are the skinning pass's output
    // (VulkanSkinningPass), from the bind pose vertices and the skin (VertexSkin, 24 bytes a vertex).
    bool IsSkinned() const
    {
        return m_skinned;
    }
    // Skinned or a tyre (MeshData::IsPosed): the skinning pass writes the vertex and position buffers
    // every frame from the bind pose.
    bool IsPosed() const
    {
        return m_posed;
    }
    VkBuffer GetBindPoseHandle() const
    {
        return m_bindPoseBuffer;
    }
    VkBuffer GetSkinHandle() const
    {
        return m_skinBuffer;
    }
    // Where each vertex was last frame, before the entity's own motion: a posed mesh's last pose (the
    // skinning pass keeps it), anyone else's position stream.
    VkBuffer GetPreviousPositionHandle() const
    {
        return m_posed ? m_previousPositionBuffer : m_positionBuffer;
    }
    // A posed, device-addressable mesh's position stream, which its ray tracing bottom level is
    // built and refitted from (VulkanRayAcceleration); 0 otherwise.
    VkDeviceAddress GetPositionAddress() const
    {
        return m_positionAddress;
    }

  private:
    // Shared by the destructor and the constructors' unwind path. Skips null handles.
    void DestroyHandles();
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
    // A device-local buffer bound to a range of VulkanMemoryPool's shared memory.
    void CreateDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VulkanPooledMemory& memory, VkDeviceAddress* address = nullptr);
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
        VulkanPooledMemory& memory,
        VkDeviceAddress* address = nullptr);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    // Only the counts outlive the upload. The mesh itself is owned by the model cache and
    // staged straight from there, so the GPU buffers don't shadow a second host-side copy.
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    bool m_deviceAddressable = false;
    VkDeviceAddress m_vertexAddress = 0;
    VkDeviceAddress m_indexAddress = 0;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_vertexMemory;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_indexMemory;
    VkBuffer m_positionBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_positionMemory;
    // A posed mesh's bind pose vertices and a skinned one's skin, which the skinning pass reads to
    // write the vertex and position buffers above (storage buffers too, then).
    bool m_skinned = false;
    bool m_posed = false;
    VkDeviceAddress m_positionAddress = 0;
    VkBuffer m_bindPoseBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_bindPoseMemory;
    VkBuffer m_skinBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_skinMemory;
    VkBuffer m_previousPositionBuffer = VK_NULL_HANDLE;
    VulkanPooledMemory m_previousPositionMemory;
};
}
