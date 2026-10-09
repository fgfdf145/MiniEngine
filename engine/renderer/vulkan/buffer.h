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

// A mesh's device-local buffers, made by NVRHI and bound to ranges of VulkanMemoryPool's heaps. The
// recording code is still Vulkan's, so the getters hand out the native handles.
class VulkanBuffer
{
  public:
    // Records this buffer's vertex/index upload into a caller-supplied batch instead of
    // submitting and waiting on its own. The caller must call uploadBatch.Flush() (directly or
    // via destruction) before the buffers are used, and keep the batch alive until then.
    // deviceAddressable (needs bufferDeviceAddress): the vertex and index buffers are storage buffers
    // with device addresses too, which hardware ray tracing's hit shading reads the hit's vertices
    // through (GetVertexAddress, GetIndexAddress).
    VulkanBuffer(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
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
        return m_bindPose.native;
    }
    VkBuffer GetSkinHandle() const
    {
        return m_skin.native;
    }
    // Where each vertex was last frame, before the entity's own motion: a posed mesh's last pose (the
    // skinning pass keeps it), anyone else's position stream.
    VkBuffer GetPreviousPositionHandle() const
    {
        return m_posed ? m_previousPosition.native : m_position.native;
    }
    // The same buffers as NVRHI's, for the passes that bind them through NVRHI (the skinning pass).
    nvrhi::IBuffer* GetVertexBuffer() const
    {
        return m_vertex.handle;
    }
    nvrhi::IBuffer* GetPositionBuffer() const
    {
        return m_position.handle;
    }
    nvrhi::IBuffer* GetBindPoseBuffer() const
    {
        return m_bindPose.handle;
    }
    nvrhi::IBuffer* GetSkinBuffer() const
    {
        return m_skin.handle;
    }
    nvrhi::IBuffer* GetPreviousPositionBuffer() const
    {
        return m_posed ? m_previousPosition.handle : m_position.handle;
    }
    // A posed, device-addressable mesh's position stream, which its ray tracing bottom level is
    // built and refitted from (VulkanRayAcceleration); 0 otherwise.
    VkDeviceAddress GetPositionAddress() const
    {
        return m_positionAddress;
    }

  private:
    // One device-local buffer: NVRHI's, created virtual and bound to a pooled range.
    struct DeviceBuffer
    {
        nvrhi::BufferHandle handle;
        VkBuffer native = VK_NULL_HANDLE;
        VulkanPooledMemory memory;
    };

    // Shared by the destructor and the constructors' unwind path. Skips empty buffers.
    void DestroyHandles();
    // A host-visible staging buffer of its own, for a batch that cannot stage (native until the
    // upload batch moves to NVRHI).
    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
    // usage is the buffer's Vulkan usage beyond the transfers, which NVRHI gives every buffer.
    void CreateDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage, DeviceBuffer& buffer, VkDeviceAddress* address = nullptr);
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
        DeviceBuffer& buffer,
        VkDeviceAddress* address = nullptr);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    // Only the counts outlive the upload. The mesh itself is owned by the model cache and
    // staged straight from there, so the GPU buffers don't shadow a second host-side copy.
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    bool m_deviceAddressable = false;
    VkDeviceAddress m_vertexAddress = 0;
    VkDeviceAddress m_indexAddress = 0;
    DeviceBuffer m_vertex;
    DeviceBuffer m_index;
    DeviceBuffer m_position;
    // A posed mesh's bind pose vertices and a skinned one's skin, which the skinning pass reads to
    // write the vertex and position buffers above (storage buffers too, then).
    bool m_skinned = false;
    bool m_posed = false;
    VkDeviceAddress m_positionAddress = 0;
    DeviceBuffer m_bindPose;
    DeviceBuffer m_skin;
    DeviceBuffer m_previousPosition;
};
}
