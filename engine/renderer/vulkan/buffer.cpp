#include "buffer.h"

#include <log/log.h>

#include <cstddef>
#include <cstring>

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
    VkQueue graphicsQueue
)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    const MeshData defaultMesh = CreateDefaultCubeMesh();
    m_vertices = defaultMesh.vertices;
    m_indices = defaultMesh.indices;

    VulkanUploadBatch uploadBatch(device, graphicsQueueFamily, graphicsQueue);
    UploadVertices(uploadBatch);
    UploadIndices(uploadBatch);
    uploadBatch.Flush();
    LOG_INFO("Vertex buffer created successfully");
}

VulkanBuffer::VulkanBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const MeshData& meshData,
    VulkanUploadBatch& uploadBatch
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_vertices(meshData.vertices),
      m_indices(meshData.indices)
{
    UploadVertices(uploadBatch);
    UploadIndices(uploadBatch);
}

VulkanBuffer::~VulkanBuffer()
{
    if (m_indexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
    }
    if (m_indexMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_indexMemory, nullptr);
    }
    if (m_vertexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
    }
    if (m_vertexMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_vertexMemory, nullptr);
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
    return static_cast<uint32_t>(m_vertices.size());
}

uint32_t VulkanBuffer::GetIndexCount() const
{
    return static_cast<uint32_t>(m_indices.size());
}

void VulkanBuffer::CreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory
)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create Vulkan buffer");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, properties);

    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate Vulkan buffer memory");
    CheckVulkan(vkBindBufferMemory(m_device, buffer, memory, 0), "Failed to bind Vulkan buffer memory");
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

void VulkanBuffer::UploadVertices(VulkanUploadBatch& uploadBatch)
{
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(sizeof(Vertex) * m_vertices.size());

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory
    );

    void* data = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data), "Failed to map staging buffer memory");
    std::memcpy(data, m_vertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_vertexBuffer,
        m_vertexMemory
    );

    VkBufferCopy copyRegion{};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(uploadBatch.GetCommandBuffer(), stagingBuffer, m_vertexBuffer, 1, &copyRegion);

    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);
}

void VulkanBuffer::UploadIndices(VulkanUploadBatch& uploadBatch)
{
    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(sizeof(uint32_t) * m_indices.size());

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer,
        stagingMemory
    );

    void* data = nullptr;
    CheckVulkan(vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data), "Failed to map index staging buffer memory");
    std::memcpy(data, m_indices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_indexBuffer,
        m_indexMemory
    );

    VkBufferCopy copyRegion{};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(uploadBatch.GetCommandBuffer(), stagingBuffer, m_indexBuffer, 1, &copyRegion);

    uploadBatch.TrackStagingResource(stagingBuffer, stagingMemory);
}
