#include "buffer.h"

#include <log/log.h>

#include <cstddef>
#include <cstring>

namespace
{
const std::array<Vertex, 3> kTriangleVertices = {{
    {{ 0.0f, -0.65f }, { 1.0f, 0.1f, 0.1f }},
    {{ 0.65f, 0.55f }, { 0.1f, 1.0f, 0.1f }},
    {{ -0.65f, 0.55f }, { 0.1f, 0.4f, 1.0f }}
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
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = static_cast<uint32_t>(offsetof(Vertex, position));

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = static_cast<uint32_t>(offsetof(Vertex, color));

    return attributeDescriptions;
}

VulkanBuffer::VulkanBuffer(VkPhysicalDevice physicalDevice, VkDevice device)
    : m_physicalDevice(physicalDevice), m_device(device)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(kTriangleVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_buffer), "Failed to create vertex buffer");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_device, m_buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_memory), "Failed to allocate vertex buffer memory");
    CheckVulkan(vkBindBufferMemory(m_device, m_buffer, m_memory, 0), "Failed to bind vertex buffer memory");

    UploadVertices();
    LOG_INFO("Vertex buffer created successfully");
}

VulkanBuffer::~VulkanBuffer()
{
    if (m_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_memory, nullptr);
    }
    if (m_buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_buffer, nullptr);
    }
}

VkBuffer VulkanBuffer::GetHandle() const
{
    return m_buffer;
}

uint32_t VulkanBuffer::GetVertexCount() const
{
    return static_cast<uint32_t>(kTriangleVertices.size());
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
    void* data = nullptr;
    CheckVulkan(vkMapMemory(m_device, m_memory, 0, sizeof(kTriangleVertices), 0, &data), "Failed to map vertex buffer memory");
    std::memcpy(data, kTriangleVertices.data(), sizeof(kTriangleVertices));
    vkUnmapMemory(m_device, m_memory);
}
