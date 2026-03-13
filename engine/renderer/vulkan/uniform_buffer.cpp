#include "uniform_buffer.h"

#include <array>
#include <cstring>
#include <glm/geometric.hpp>

VulkanUniformBuffer::VulkanUniformBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t imageCount,
    const std::vector<MaterialTextureBinding>& materialBindings
)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_materialBindings(materialBindings),
      m_imageCount(imageCount)
{
    if (m_materialBindings.empty())
    {
        throw std::runtime_error("Uniform buffer requires at least one material binding");
    }

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

VkDescriptorSet VulkanUniformBuffer::GetDescriptorSet(uint32_t imageIndex, uint32_t materialIndex) const
{
    if (imageIndex >= m_imageCount)
    {
        throw std::runtime_error("Descriptor set image index is out of range");
    }
    if (materialIndex >= m_materialBindings.size())
    {
        throw std::runtime_error("Descriptor set material index is out of range");
    }

    const size_t descriptorIndex =
        static_cast<size_t>(imageIndex) * m_materialBindings.size() + materialIndex;
    return m_descriptorSets[descriptorIndex];
}

void VulkanUniformBuffer::Update(uint32_t imageIndex, const ViewportMatrices& matrices, const glm::vec3& cameraPosition)
{
    CameraUniformData data{};
    data.view = matrices.view;
    data.proj = matrices.renderProjection;
    data.cameraWorldPosition = glm::vec4(cameraPosition, 1.0f);

    const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.35f));
    data.lightDirectionAndIntensity = glm::vec4(lightDirection, 2.25f);
    data.lightColorAndAmbient = glm::vec4(1.0f, 0.98f, 0.95f, 0.2f);
    std::memcpy(m_mappedBuffers[imageIndex], &data, sizeof(data));
}

void VulkanUniformBuffer::CreateDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding uniformBinding{};
    uniformBinding.binding = 0;
    uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformBinding.descriptorCount = 1;
    uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    bindings[0] = uniformBinding;

    for (uint32_t bindingIndex = 1; bindingIndex < static_cast<uint32_t>(bindings.size()); ++bindingIndex)
    {
        bindings[bindingIndex].binding = bindingIndex;
        bindings[bindingIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[bindingIndex].descriptorCount = 1;
        bindings[bindingIndex].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

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
    const uint32_t descriptorSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptorSetCount },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptorSetCount * 6 }
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = descriptorSetCount;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create uniform descriptor pool");
}

void VulkanUniformBuffer::CreateDescriptorSets(uint32_t imageCount)
{
    const uint32_t descriptorSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = descriptorSetCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(descriptorSetCount);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate uniform descriptor sets");

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CameraUniformData);

        for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(m_materialBindings.size()); ++materialIndex)
        {
            const MaterialTextureBinding& materialBinding = m_materialBindings[materialIndex];

            const size_t descriptorIndex =
                static_cast<size_t>(i) * m_materialBindings.size() + materialIndex;

            const std::array<TextureDescriptorBinding, 6> textureBindings = {
                materialBinding.baseColor,
                materialBinding.normal,
                materialBinding.metallic,
                materialBinding.roughness,
                materialBinding.occlusion,
                materialBinding.emissive
            };

            std::array<VkDescriptorImageInfo, 6> imageInfos{};
            for (size_t textureBindingIndex = 0; textureBindingIndex < textureBindings.size(); ++textureBindingIndex)
            {
                imageInfos[textureBindingIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[textureBindingIndex].imageView = textureBindings[textureBindingIndex].imageView;
                imageInfos[textureBindingIndex].sampler = textureBindings[textureBindingIndex].sampler;
            }

            std::array<VkWriteDescriptorSet, 7> descriptorWrites{};
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = m_descriptorSets[descriptorIndex];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            for (uint32_t bindingIndex = 1; bindingIndex < static_cast<uint32_t>(descriptorWrites.size()); ++bindingIndex)
            {
                descriptorWrites[bindingIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrites[bindingIndex].dstSet = m_descriptorSets[descriptorIndex];
                descriptorWrites[bindingIndex].dstBinding = bindingIndex;
                descriptorWrites[bindingIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrites[bindingIndex].descriptorCount = 1;
                descriptorWrites[bindingIndex].pImageInfo = &imageInfos[bindingIndex - 1];
            }

            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        }
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
