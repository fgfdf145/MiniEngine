#include "buffer.h"

#include <engine/core/log/log.h>

#include <cstddef>
#include <cstring>

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

std::array<VkVertexInputAttributeDescription, 5> GetVertexAttributeDescriptions()
{
    std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions{};

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

    return attributeDescriptions;
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
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_vertexCount(static_cast<uint32_t>(meshData.vertices.size())),
      m_indexCount(static_cast<uint32_t>(meshData.indices.size()))
{
    // A throw out of a constructor skips the destructor, so whatever was created before the
    // failure is released here with the same call the destructor makes. The copies recorded into
    // the batch are never submitted in that case: the batch is abandoned along with this buffer.
    try
    {
        UploadVertices(meshData, uploadBatch);
        UploadIndices(meshData, uploadBatch);
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
    if (m_indexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    if (m_indexMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_indexMemory, nullptr);
        m_indexMemory = VK_NULL_HANDLE;
    }
    if (m_vertexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_vertexMemory, nullptr);
        m_vertexMemory = VK_NULL_HANDLE;
    }
}

VkBuffer VulkanBuffer::GetVertexHandle() const
{
    return m_vertexBuffer;
}

VkBuffer VulkanBuffer::GetIndexHandle() const
{
    return m_indexBuffer;
}

uint32_t VulkanBuffer::GetVertexCount() const
{
    return m_vertexCount;
}

uint32_t VulkanBuffer::GetIndexCount() const
{
    return m_indexCount;
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

void VulkanBuffer::UploadVertices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(sizeof(Vertex) * meshData.vertices.size());

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory);
    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

    void* data = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data), "Failed to map staging buffer memory");
    std::memcpy(data, meshData.vertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_vertexBuffer,
        m_vertexMemory);

    VkBufferCopy copyRegion{};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(uploadBatch.GetCommandBuffer(), stagingBuffer, m_vertexBuffer, 1, &copyRegion);
}

void VulkanBuffer::UploadIndices(const MeshData& meshData, VulkanUploadBatch& uploadBatch)
{
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(sizeof(uint32_t) * meshData.indices.size());

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory);
    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);

    void* data = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data), "Failed to map index staging buffer memory");
    std::memcpy(data, meshData.indices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_indexBuffer,
        m_indexMemory);

    VkBufferCopy copyRegion{};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(uploadBatch.GetCommandBuffer(), stagingBuffer, m_indexBuffer, 1, &copyRegion);
}
}
