#include "buffer.h"
#include "memory_pool.h"

#include <engine/core/log/log.h>

#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>
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
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    const MeshData defaultMesh = CreateDefaultCubeMesh();
    m_vertexCount = static_cast<uint32_t>(defaultMesh.vertices.size());
    m_indexCount = static_cast<uint32_t>(defaultMesh.indices.size());

    try
    {
        VulkanUploadBatch uploadBatch(device, graphicsQueueFamily, graphicsQueue);
        UploadVertices(defaultMesh, uploadBatch);
        UploadIndices(defaultMesh, uploadBatch);
        UploadPositions(defaultMesh, uploadBatch);
        uploadBatch.Flush();
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
    LOG_INFO("Vertex buffer created successfully");
}

VulkanBuffer::VulkanBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const MeshData& meshData,
    VulkanUploadBatch& uploadBatch,
    bool deviceAddressable)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_vertexCount(static_cast<uint32_t>(meshData.vertices.size())),
      m_indexCount(static_cast<uint32_t>(meshData.indices.size())),
      m_deviceAddressable(deviceAddressable),
      m_skinned(meshData.IsSkinned() && meshData.skin.size() == meshData.vertices.size())
{
    // A throw out of a constructor skips the destructor, so whatever was created before the
    // failure is released here with the same call the destructor makes. The copies recorded into
    // the batch are never submitted in that case: the batch is abandoned along with this buffer.
    try
    {
        UploadVertices(meshData, uploadBatch);
        UploadIndices(meshData, uploadBatch);
        UploadPositions(meshData, uploadBatch);
        if (m_skinned)
        {
            static_assert(sizeof(VertexSkin) == 24, "skin.comp reads VertexSkin as six words");
            UploadDeviceLocal(
                meshData.vertices.data(),
                static_cast<VkDeviceSize>(sizeof(Vertex) * meshData.vertices.size()),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                uploadBatch,
                m_bindPoseBuffer,
                m_bindPoseMemory);
            UploadDeviceLocal(
                meshData.skin.data(),
                static_cast<VkDeviceSize>(sizeof(VertexSkin) * meshData.skin.size()),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                uploadBatch,
                m_skinBuffer,
                m_skinMemory);
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
    for (auto [buffer, memory] : {std::pair<VkBuffer*, VulkanPooledMemory*>{&m_bindPoseBuffer, &m_bindPoseMemory},
                                  std::pair<VkBuffer*, VulkanPooledMemory*>{&m_skinBuffer, &m_skinMemory},
                                  std::pair<VkBuffer*, VulkanPooledMemory*>{&m_previousPositionBuffer, &m_previousPositionMemory}})
    {
        if (*buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
        }
        VulkanMemoryPool::Free(m_device, *memory);
    }
    if (m_positionBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_positionBuffer, nullptr);
        m_positionBuffer = VK_NULL_HANDLE;
    }
    VulkanMemoryPool::Free(m_device, m_positionMemory);
    if (m_indexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    VulkanMemoryPool::Free(m_device, m_indexMemory);
    if (m_vertexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    VulkanMemoryPool::Free(m_device, m_vertexMemory);
}

VkBuffer VulkanBuffer::GetVertexHandle() const
{
    return m_vertexBuffer;
}

VkBuffer VulkanBuffer::GetIndexHandle() const
{
    return m_indexBuffer;
}

VkBuffer VulkanBuffer::GetPositionHandle() const
{
    return m_positionBuffer;
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

void VulkanBuffer::CreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create Vulkan buffer");

    // Either both handles come back valid or neither does, so no caller has to clean up after a
    // half-built buffer: running out of memory here is expected on large scenes.
    try
    {
        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, properties);

        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate Vulkan buffer memory");
        CheckVulkan(vkBindBufferMemory(m_device, buffer, memory, 0), "Failed to bind Vulkan buffer memory");
    }
    catch (...)
    {
        vkFreeMemory(m_device, memory, nullptr);
        memory = VK_NULL_HANDLE;
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        throw;
    }
}

void VulkanBuffer::CreateDeviceLocalBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VulkanPooledMemory& memory,
    VkDeviceAddress* address)
{
    if (address != nullptr)
    {
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create Vulkan buffer");

    // As CreateBuffer: both handles come back valid or neither does.
    try
    {
        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);
        memory = VulkanMemoryPool::Allocate(
            m_physicalDevice,
            m_device,
            memoryRequirements,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            address != nullptr ? VulkanMemoryPool::Resource::AddressableBuffer : VulkanMemoryPool::Resource::Buffer);
        CheckVulkan(vkBindBufferMemory(m_device, buffer, memory.memory, memory.offset), "Failed to bind Vulkan buffer memory");
        if (address != nullptr)
        {
            VkBufferDeviceAddressInfo addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addressInfo.buffer = buffer;
            *address = vkGetBufferDeviceAddress(m_device, &addressInfo);
        }
    }
    catch (...)
    {
        VulkanMemoryPool::Free(m_device, memory);
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        throw;
    }
}

uint32_t VulkanBuffer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertiesMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertiesMatch)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable vertex buffer memory type");
}

void VulkanBuffer::UploadDeviceLocal(
    const void* source,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VulkanUploadBatch& uploadBatch,
    VkBuffer& buffer,
    VulkanPooledMemory& memory,
    VkDeviceAddress* address)
{
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    if (uploadBatch.CanStage())
    {
        // The batch's shared staging chunks: one allocation per resource cost a scene of tens of
        // thousands of submeshes a third of a millisecond each.
        const VulkanUploadBatch::StagingSlice slice = uploadBatch.Stage(source, size);
        stagingBuffer = slice.buffer;
        copyRegion.srcOffset = slice.offset;
    }
    else
    {
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer,
            stagingMemory);
        uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

        void* data = nullptr;
        CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, size, 0, &data), "Failed to map staging buffer memory");
        std::memcpy(data, source, static_cast<size_t>(size));
        vkUnmapMemory(m_device, stagingMemory);
    }

    CreateDeviceLocalBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage, buffer, memory, address);
    vkCmdCopyBuffer(uploadBatch.GetCommandBuffer(), stagingBuffer, buffer, 1, &copyRegion);
}

void VulkanBuffer::UploadVertices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    UploadDeviceLocal(
        meshData.vertices.data(),
        static_cast<VkDeviceSize>(sizeof(Vertex) * meshData.vertices.size()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_skinned ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u),
        uploadBatch,
        m_vertexBuffer,
        m_vertexMemory,
        m_deviceAddressable ? &m_vertexAddress : nullptr);
}

void VulkanBuffer::UploadIndices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    UploadDeviceLocal(
        meshData.indices.data(),
        static_cast<VkDeviceSize>(sizeof(uint32_t) * meshData.indices.size()),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        uploadBatch,
        m_indexBuffer,
        m_indexMemory,
        m_deviceAddressable ? &m_indexAddress : nullptr);
}

void VulkanBuffer::UploadPositions(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    std::vector<float> positions;
    positions.reserve(meshData.vertices.size() * 3);
    for (const Vertex& vertex : meshData.vertices)
    {
        positions.insert(positions.end(), std::begin(vertex.position), std::end(vertex.position));
    }
    if (m_skinned)
    {
        // Last frame's pose, which the skinning pass rolls the positions into before posing them anew.
        UploadDeviceLocal(
            positions.data(),
            static_cast<VkDeviceSize>(sizeof(float) * positions.size()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploadBatch,
            m_previousPositionBuffer,
            m_previousPositionMemory);
    }
    UploadDeviceLocal(
        positions.data(),
        static_cast<VkDeviceSize>(sizeof(float) * positions.size()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_skinned ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u) |
            (m_skinned && m_deviceAddressable ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0u),
        uploadBatch,
        m_positionBuffer,
        m_positionMemory,
        m_skinned && m_deviceAddressable ? &m_positionAddress : nullptr);
}
}
