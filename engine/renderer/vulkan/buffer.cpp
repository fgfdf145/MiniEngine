#include "buffer.h"

#if MINIENGINE_WITH_D3D12
#include "d3d12_buffer_views.h"
#endif
#include "memory_pool.h"
#include "nvrhi_native.h"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

namespace me
{

VkVertexInputBindingDescription GetVertexBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

std::array<VkVertexInputAttributeDescription, 6> GetVertexAttributeDescriptions()
{
    std::array<VkVertexInputAttributeDescription, 6> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = static_cast<uint32_t>(offsetof(Vertex, position));

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = static_cast<uint32_t>(offsetof(Vertex, color));

    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[2].offset = static_cast<uint32_t>(offsetof(Vertex, texCoord));

    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[3].offset = static_cast<uint32_t>(offsetof(Vertex, normal));

    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 4;
    attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[4].offset = static_cast<uint32_t>(offsetof(Vertex, tangent));

    attributeDescriptions[5].binding = 0;
    attributeDescriptions[5].location = 5;
    attributeDescriptions[5].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[5].offset = static_cast<uint32_t>(offsetof(Vertex, texCoord1));

    return attributeDescriptions;
}

std::array<VkVertexInputAttributeDescription, 6> GetToonVertexAttributeDescriptions()
{
    const std::array<VkVertexInputAttributeDescription, 6> common = GetVertexAttributeDescriptions();
    // Position, UV 0, normal and UV 1 as every material pipeline reads them; no colour or tangent.
    std::array<VkVertexInputAttributeDescription, 6> attributeDescriptions = {
        common[0], common[2], common[3], common[5], {}, GetPreviousPositionAttributeDescription()};
    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 6;
    attributeDescriptions[4].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[4].offset = static_cast<uint32_t>(offsetof(Vertex, outlineNormal));
    return attributeDescriptions;
}

VkVertexInputBindingDescription GetPreviousPositionBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 1;
    bindingDescription.stride = sizeof(float) * 3;
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

VkVertexInputAttributeDescription GetPreviousPositionAttributeDescription()
{
    VkVertexInputAttributeDescription attributeDescription{};
    attributeDescription.binding = 1;
    attributeDescription.location = 7;
    attributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescription.offset = 0;
    return attributeDescription;
}

VkVertexInputBindingDescription GetPositionBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(float) * 3;
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

VkVertexInputAttributeDescription GetPositionAttributeDescription()
{
    VkVertexInputAttributeDescription attributeDescription{};
    attributeDescription.binding = 0;
    attributeDescription.location = 0;
    attributeDescription.format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescription.offset = 0;
    return attributeDescription;
}

VulkanBuffer::VulkanBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const MeshData& meshData,
    VulkanUploadBatch& uploadBatch,
    bool deviceAddressable)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_vertexCount(static_cast<uint32_t>(meshData.vertices.size())),
      m_indexCount(static_cast<uint32_t>(meshData.indices.size())),
      m_deviceAddressable(deviceAddressable),
      m_skinned(meshData.IsSkinned() && meshData.skin.size() == meshData.vertices.size()),
      m_posed(m_skinned || meshData.deformable)
{
    // A throw out of a constructor skips the destructor, so whatever was created before the
    // failure is released here with the same call the destructor makes. The copies recorded into
    // the batch are never submitted in that case: the batch is abandoned along with this buffer.
    try
    {
        UploadVertices(meshData, uploadBatch);
        UploadIndices(meshData, uploadBatch);
        UploadPositions(meshData, uploadBatch);
        if (m_posed)
        {
            UploadDeviceLocal(
                meshData.vertices.data(),
                static_cast<VkDeviceSize>(sizeof(Vertex) * meshData.vertices.size()),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                uploadBatch,
                m_bindPose);
        }
        if (m_skinned)
        {
            static_assert(sizeof(VertexSkin) == 24, "skin.comp reads VertexSkin as six words");
            UploadDeviceLocal(
                meshData.skin.data(),
                static_cast<VkDeviceSize>(sizeof(VertexSkin) * meshData.skin.size()),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                uploadBatch,
                m_skin);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanBuffer::~VulkanBuffer()
{
    DestroyHandles();
}

void VulkanBuffer::DestroyHandles()
{
#if MINIENGINE_WITH_D3D12
    for (uint32_t* view : {&m_vertexView, &m_indexView})
    {
        ReleaseD3D12RawBufferView(*view);
        *view = kNoD3D12BufferView;
    }
#endif
    // The buffers go before the ranges they are bound to.
    for (DeviceBuffer* buffer : {&m_bindPose, &m_skin, &m_previousPosition, &m_position, &m_index, &m_vertex})
    {
        buffer->handle = nullptr;
        buffer->native = VK_NULL_HANDLE;
        VulkanMemoryPool::Free(buffer->memory);
    }
}

VkBuffer VulkanBuffer::GetVertexHandle() const
{
    return m_vertex.native;
}

VkBuffer VulkanBuffer::GetIndexHandle() const
{
    return m_index.native;
}

VkBuffer VulkanBuffer::GetPositionHandle() const
{
    return m_position.native;
}

uint32_t VulkanBuffer::GetVertexCount() const
{
    return m_vertexCount;
}

uint32_t VulkanBuffer::GetIndexCount() const
{
    return m_indexCount;
}

VkDeviceAddress VulkanBuffer::GetVertexAddress() const
{
    return m_vertexAddress;
}

VkDeviceAddress VulkanBuffer::GetIndexAddress() const
{
    return m_indexAddress;
}

void VulkanBuffer::CreateDeviceLocalBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    DeviceBuffer& buffer,
    VkDeviceAddress* address)
{
    if (address != nullptr)
    {
        // Read through its device address by the hit shading; NVRHI gives every buffer an address
        // when the device has them.
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    nvrhi::BufferDesc desc;
    desc.byteSize = size;
    desc.isVertexBuffer = (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0;
    desc.isIndexBuffer = (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0;
    desc.canHaveUAVs = (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
    desc.canHaveRawViews = desc.canHaveUAVs;
    desc.isAccelStructBuildInput = (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) != 0;
    desc.isVirtual = true;
    // The skinning pass and the hit shading read and write the posed buffers as floats and words
    // (StructuredBuffer<float>, <uint>).
    desc.structStride = desc.canHaveUAVs ? 4 : 0;
    desc.debugName = "Mesh buffer";
    // At rest every reader's state at once: vertex input, the hit shading's and the skinning's reads,
    // the ray tracing builds. Only the upload and the skinning's writes move it out (and back).
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    if (desc.isVertexBuffer)
    {
        desc.initialState = desc.initialState | nvrhi::ResourceStates::VertexBuffer;
    }
    if (desc.isIndexBuffer)
    {
        desc.initialState = desc.initialState | nvrhi::ResourceStates::IndexBuffer;
    }
    if (desc.isAccelStructBuildInput)
    {
        desc.initialState = desc.initialState | nvrhi::ResourceStates::AccelStructBuildInput;
    }
    desc.keepInitialState = true;
    buffer.handle = m_nvrhiDevice->createBuffer(desc);
    if (!buffer.handle)
    {
        throw std::runtime_error("Failed to create a mesh buffer");
    }
    buffer.native = ToNative<VkBuffer>(buffer.handle->getNativeObject(nvrhi::ObjectTypes::VK_Buffer));

    // The buffer comes back whole or not at all.
    try
    {
        buffer.memory = VulkanMemoryPool::AllocateFor(
            m_nvrhiDevice,
            buffer.handle,
            address != nullptr ? VulkanMemoryPool::Resource::AddressableBuffer : VulkanMemoryPool::Resource::Buffer);
        if (!m_nvrhiDevice->bindBufferMemory(buffer.handle, buffer.memory.heap, buffer.memory.offset))
        {
            throw std::runtime_error("Failed to bind mesh buffer memory");
        }
        if (address != nullptr)
        {
            *address = buffer.handle->getGpuVirtualAddress();
        }
    }
    catch (...)
    {
        buffer.handle = nullptr;
        buffer.native = VK_NULL_HANDLE;
        VulkanMemoryPool::Free(buffer.memory);
        throw;
    }
}


void VulkanBuffer::UploadDeviceLocal(
    const void* source,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VulkanUploadBatch& uploadBatch,
    DeviceBuffer& buffer,
    VkDeviceAddress* address)
{
    CreateDeviceLocalBuffer(size, usage, buffer, address);
    uploadBatch.WriteBuffer(buffer.handle, source, size);
}

void VulkanBuffer::UploadVertices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    UploadDeviceLocal(
        meshData.vertices.data(),
        static_cast<VkDeviceSize>(sizeof(Vertex) * meshData.vertices.size()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_posed ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u),
        uploadBatch,
        m_vertex,
        m_deviceAddressable ? &m_vertexAddress : nullptr);
    CreateHitShadingView(m_vertex, m_vertexView, m_vertexAddress);
}

void VulkanBuffer::UploadIndices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    UploadDeviceLocal(
        meshData.indices.data(),
        static_cast<VkDeviceSize>(sizeof(uint32_t) * meshData.indices.size()),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        uploadBatch,
        m_index,
        m_deviceAddressable ? &m_indexAddress : nullptr);
    CreateHitShadingView(m_index, m_indexView, m_indexAddress);
}

void VulkanBuffer::CreateHitShadingView(const DeviceBuffer& buffer, uint32_t& view, VkDeviceAddress& address)
{
#if MINIENGINE_WITH_D3D12
    if (m_deviceAddressable && m_nvrhiDevice->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        view = CreateD3D12RawBufferView(buffer.handle);
        address = view;
    }
#else
    (void)buffer;
    (void)view;
    (void)address;
#endif
}

void VulkanBuffer::UploadPositions(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    std::vector<float> positions;
    positions.reserve(meshData.vertices.size() * 3);
    for (const Vertex& vertex : meshData.vertices)
    {
        positions.insert(positions.end(), std::begin(vertex.position), std::end(vertex.position));
    }
    if (m_posed)
    {
        // Last frame's pose, which the skinning pass rolls the positions into before posing them anew.
        UploadDeviceLocal(
            positions.data(),
            static_cast<VkDeviceSize>(sizeof(float) * positions.size()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploadBatch,
            m_previousPosition);
    }
    UploadDeviceLocal(
        positions.data(),
        static_cast<VkDeviceSize>(sizeof(float) * positions.size()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_posed ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u) |
            (m_posed && m_deviceAddressable ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0u),
        uploadBatch,
        m_position,
        m_posed && m_deviceAddressable ? &m_positionAddress : nullptr);
}
}
