#include "buffer.h"

#include <log/log.h>

#include <cstddef>
#include <cstring>

namespace
{
const std::array<Vertex, 8> kTriangleVertices = {{
    {{ -0.75f, -0.75f, -0.75f }, { 1.0f, 0.2f, 0.2f }},
    {{ 0.75f, -0.75f, -0.75f }, { 0.2f, 1.0f, 0.2f }},
    {{ 0.75f, 0.75f, -0.75f }, { 0.2f, 0.2f, 1.0f }},
    {{ -0.75f, 0.75f, -0.75f }, { 1.0f, 1.0f, 0.2f }},
    {{ -0.75f, -0.75f, 0.75f }, { 1.0f, 0.2f, 1.0f }},
    {{ 0.75f, -0.75f, 0.75f }, { 0.2f, 1.0f, 1.0f }},
    {{ 0.75f, 0.75f, 0.75f }, { 0.9f, 0.9f, 0.9f }},
    {{ -0.75f, 0.75f, 0.75f }, { 0.3f, 0.3f, 0.3f }}
}};

const std::array<uint16_t, 36> kTriangleIndices = {{
    0, 1, 2, 2, 3, 0,
    4, 5, 6, 6, 7, 4,
    0, 4, 7, 7, 3, 0,
    1, 5, 6, 6, 2, 1,
    3, 2, 6, 6, 7, 3,
    0, 1, 5, 5, 4, 0
}};
}

VkVertexInputBindingDescription Vertex::GetBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return bindingDescription;
}

std::array<VkVertexInputAttributeDescription, 2> Vertex::GetAttributeDescriptions()
{
    std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = static_cast<uint32_t>(offsetof(Vertex, position));

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = static_cast<uint32_t>(offsetof(Vertex, color));

    return attributeDescriptions;
}

VulkanBuffer::VulkanBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_graphicsQueueFamily(graphicsQueueFamily),
      m_graphicsQueue(graphicsQueue)
{
    UploadVertices();
    UploadIndices();
    LOG_INFO("Vertex buffer created successfully");
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
    return static_cast<uint32_t>(kTriangleVertices.size());
}

uint32_t VulkanBuffer::GetIndexCount() const
{
    return static_cast<uint32_t>(kTriangleIndices.size());
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

void VulkanBuffer::CopyBuffer(VkBuffer sourceBuffer, VkBuffer destinationBuffer, VkDeviceSize size) const
{
    VkCommandPool commandPool = VK_NULL_HANDLE;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &commandPool), "Failed to create staging command pool");

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer), "Failed to allocate staging command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin staging command buffer");

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, sourceBuffer, destinationBuffer, 1, &copyRegion);

    CheckVulkan(vkEndCommandBuffer(commandBuffer), "Failed to end staging command buffer");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    CheckVulkan(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit staging copy");
    CheckVulkan(vkQueueWaitIdle(m_graphicsQueue), "Failed to wait for staging copy");

    vkDestroyCommandPool(m_device, commandPool, nullptr);
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

void VulkanBuffer::UploadVertices()
{
    const VkDeviceSize bufferSize = sizeof(kTriangleVertices);

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
    std::memcpy(data, kTriangleVertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_vertexBuffer,
        m_vertexMemory
    );

    CopyBuffer(stagingBuffer, m_vertexBuffer, bufferSize);

    vkFreeMemory(m_device, stagingMemory, nullptr);
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
}

void VulkanBuffer::UploadIndices()
{
    const VkDeviceSize bufferSize = sizeof(kTriangleIndices);

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
    std::memcpy(data, kTriangleIndices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, stagingMemory);

    CreateBuffer(
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_indexBuffer,
        m_indexMemory
    );

    CopyBuffer(stagingBuffer, m_indexBuffer, bufferSize);

    vkFreeMemory(m_device, stagingMemory, nullptr);
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
}
