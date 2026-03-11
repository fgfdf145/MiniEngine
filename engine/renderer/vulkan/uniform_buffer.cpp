#include "uniform_buffer.h"

#include <cstring>

VulkanUniformBuffer::VulkanUniformBuffer(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t imageCount)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    CreateDescriptorSetLayout();
    CreateBuffers(imageCount);
    CreateDescriptorPool(imageCount);
    CreateDescriptorSets(imageCount);
}

VulkanUniformBuffer::~VulkanUniformBuffer()
{
    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        if (m_mappedBuffers[i] != nullptr)
        {
            vkUnmapMemory(m_device, m_memories[i]);
        }
        if (m_buffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_buffers[i], nullptr);
        }
        if (m_memories[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_memories[i], nullptr);
        }
    }

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
    }
}

VkDescriptorSetLayout VulkanUniformBuffer::GetDescriptorSetLayout() const
{
    return m_descriptorSetLayout;
}

VkDescriptorSet VulkanUniformBuffer::GetDescriptorSet(uint32_t imageIndex) const
{
    return m_descriptorSets[imageIndex];
}

void VulkanUniformBuffer::Update(uint32_t imageIndex, const Camera& camera, VkExtent2D extent)
{
    CameraUniformData data{};
    data.model = camera.GetModelMatrix();
    data.view = camera.GetViewMatrix();
    data.proj = camera.GetProjectionMatrix(extent);
    std::memcpy(m_mappedBuffers[imageIndex], &data, sizeof(data));
}

void VulkanUniformBuffer::CreateDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout), "Failed to create uniform descriptor set layout");
}

void VulkanUniformBuffer::CreateBuffers(uint32_t imageCount)
{
    m_buffers.resize(imageCount);
    m_memories.resize(imageCount);
    m_mappedBuffers.resize(imageCount);

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(CameraUniformData);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_buffers[i]), "Failed to create uniform buffer");

        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(m_device, m_buffers[i], &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_memories[i]), "Failed to allocate uniform buffer memory");
        CheckVulkan(vkBindBufferMemory(m_device, m_buffers[i], m_memories[i], 0), "Failed to bind uniform buffer memory");
        CheckVulkan(vkMapMemory(m_device, m_memories[i], 0, sizeof(CameraUniformData), 0, &m_mappedBuffers[i]), "Failed to map uniform buffer memory");
    }
}

void VulkanUniformBuffer::CreateDescriptorPool(uint32_t imageCount)
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = imageCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = imageCount;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create uniform descriptor pool");
}

void VulkanUniformBuffer::CreateDescriptorSets(uint32_t imageCount)
{
    std::vector<VkDescriptorSetLayout> layouts(imageCount, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = imageCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(imageCount);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate uniform descriptor sets");

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CameraUniformData);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }
}

uint32_t VulkanUniformBuffer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
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

    throw std::runtime_error("Failed to find suitable uniform buffer memory type");
}
