#include "uniform_buffer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace me
{

VulkanUniformBuffer::VulkanUniformBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t imageCount,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout materialSetLayout,
    const std::vector<MaterialTextureBinding>& materialBindings,
    TextureDescriptorBinding shadowMap,
    uint32_t motionSlotCount)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_materialBindings(materialBindings),
      m_shadowMap(shadowMap),
      m_frameSetLayout(frameSetLayout),
      m_materialSetLayout(materialSetLayout),
      // A zero-sized storage buffer is invalid, and a scene with no submeshes still binds set 0.
      m_motionSlotCount(std::max(motionSlotCount, 1u)),
      m_imageCount(imageCount)
{
    if (m_materialBindings.empty())
    {
        throw std::runtime_error("Uniform buffer requires at least one material binding");
    }

    // A content upload builds this object while the previous one is still live, so running out of
    // memory here is a recoverable failure; release whatever was created before rethrowing.
    try
    {
        CreateBuffers(imageCount);
        CreateDescriptorPool(imageCount);
        CreateDescriptorSets(imageCount);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanUniformBuffer::~VulkanUniformBuffer()
{
    DestroyHandles();
}

void VulkanUniformBuffer::DestroyHandles()
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
    for (size_t i = 0; i < m_motionBuffers.size(); ++i)
    {
        if (m_motionBuffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_motionBuffers[i], nullptr);
        }
        // Freeing mapped memory unmaps it implicitly.
        if (m_motionMemories[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_motionMemories[i], nullptr);
        }
    }

    m_buffers.clear();
    m_memories.clear();
    m_mappedBuffers.clear();
    m_motionBuffers.clear();
    m_motionMemories.clear();
    m_mappedMotionBuffers.clear();

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    // m_frameSetLayout and m_materialSetLayout are owned by VulkanFrameDescriptorSetLayout and
    // VulkanMaterialDescriptorSetLayout respectively, not by this buffer.
}

VkDescriptorSet VulkanUniformBuffer::GetFrameDescriptorSet(uint32_t imageIndex) const
{
    if (imageIndex >= m_imageCount)
    {
        throw std::runtime_error("Frame descriptor set image index is out of range");
    }

    return m_frameDescriptorSets[imageIndex];
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

void VulkanUniformBuffer::Update(
    uint32_t imageIndex,
    const ViewportMatrices& matrices,
    const glm::vec3& cameraPosition,
    const glm::vec3& ambientLuminance,
    std::span<const GpuLightData> lights,
    const ShadowUniformData& shadow,
    const glm::mat4& prevViewProj,
    std::span<const glm::mat4> prevModels)
{
    // A draw whose slot lies past the buffer would read out of bounds on the GPU, and no
    // robustness feature is enabled to catch it, so a mismatch is refused here instead.
    if (prevModels.size() > m_motionSlotCount)
    {
        throw std::runtime_error("More previous model matrices than motion slots");
    }

    CameraUniformData data{};
    data.view = matrices.view;
    data.proj = matrices.renderProjection;
    // renderProjection, not projection: it is the Y-flipped matrix the shaders actually use, and
    // inverting the other one would reconstruct every position mirrored.
    data.invViewProj = glm::inverse(matrices.renderProjection * matrices.view);
    data.cameraWorldPosition = glm::vec4(cameraPosition, 1.0f);
    data.ambientLuminance = glm::vec4(ambientLuminance, 0.0f);

    const uint32_t lightCount = std::min(static_cast<uint32_t>(lights.size()), kMaxSceneLights);
    data.sceneLightCount = glm::uvec4(lightCount, 0u, 0u, 0u);
    for (uint32_t i = 0; i < lightCount; ++i)
    {
        data.lights[i] = lights[i];
    }
    data.shadow = shadow;
    data.prevViewProj = prevViewProj;

    std::memcpy(m_mappedBuffers[imageIndex], &data, sizeof(data));
    std::memcpy(m_mappedMotionBuffers[imageIndex], prevModels.data(), prevModels.size_bytes());
}

VulkanFrameDescriptorSetLayout::VulkanFrameDescriptorSetLayout(VkDevice device)
    : m_device(device)
{
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // The shadow map, sampled with depth comparison by the material fragment shader.
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Each draw's previous model matrix, read by triangle.vert for motion vectors.
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
        "Failed to create frame descriptor set layout");
}

VulkanFrameDescriptorSetLayout::~VulkanFrameDescriptorSetLayout()
{
    if (m_layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
}

VkDescriptorSetLayout VulkanFrameDescriptorSetLayout::GetHandle() const
{
    return m_layout;
}

VulkanMaterialDescriptorSetLayout::VulkanMaterialDescriptorSetLayout(VkDevice device)
    : m_device(device)
{
    std::array<VkDescriptorSetLayoutBinding, 13> bindings{};
    for (uint32_t bindingIndex = 0; bindingIndex < static_cast<uint32_t>(bindings.size()); ++bindingIndex)
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

    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_layout),
        "Failed to create material descriptor set layout");
}

VulkanMaterialDescriptorSetLayout::~VulkanMaterialDescriptorSetLayout()
{
    if (m_layout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
}

VkDescriptorSetLayout VulkanMaterialDescriptorSetLayout::GetHandle() const
{
    return m_layout;
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
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_memories[i]), "Failed to allocate uniform buffer memory");
        CheckVulkan(vkBindBufferMemory(m_device, m_buffers[i], m_memories[i], 0), "Failed to bind uniform buffer memory");
        CheckVulkan(vkMapMemory(m_device, m_memories[i], 0, sizeof(CameraUniformData), 0, &m_mappedBuffers[i]), "Failed to map uniform buffer memory");
    }

    const VkDeviceSize motionBytes = sizeof(glm::mat4) * m_motionSlotCount;
    m_motionBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_motionMemories.assign(imageCount, VK_NULL_HANDLE);
    m_mappedMotionBuffers.assign(imageCount, nullptr);

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = motionBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_motionBuffers[i]), "Failed to create previous model buffer");

        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(m_device, m_motionBuffers[i], &memoryRequirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_motionMemories[i]), "Failed to allocate previous model buffer memory");
        CheckVulkan(vkBindBufferMemory(m_device, m_motionBuffers[i], m_motionMemories[i], 0), "Failed to bind previous model buffer memory");
        CheckVulkan(vkMapMemory(m_device, m_motionMemories[i], 0, motionBytes, 0, &m_mappedMotionBuffers[i]), "Failed to map previous model buffer memory");

        // Identity until the first Update, so nothing ever reads uninitialised memory.
        const std::vector<glm::mat4> identities(m_motionSlotCount, glm::mat4(1.0f));
        std::memcpy(m_mappedMotionBuffers[i], identities.data(), static_cast<size_t>(motionBytes));
    }
}

void VulkanUniformBuffer::CreateDescriptorPool(uint32_t imageCount)
{
    // One pool serves both sets the split produced: imageCount uniform buffers, shadow map
    // samplers and previous model buffers for set 0 and thirteen samplers per material set for
    // set 1. That is why neither its name nor its failure message belongs to either half.
    const uint32_t materialSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    const std::array<VkDescriptorPoolSize, 3> poolSizes = {{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
                                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, materialSetCount * 13 + imageCount},
                                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount}}};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = materialSetCount + imageCount;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create descriptor pool");
}

void VulkanUniformBuffer::CreateDescriptorSets(uint32_t imageCount)
{
    // Set 0: one frame descriptor set per swapchain image, allocated from the frame set layout.
    std::vector<VkDescriptorSetLayout> frameLayouts(imageCount, m_frameSetLayout);
    VkDescriptorSetAllocateInfo frameAllocateInfo{};
    frameAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    frameAllocateInfo.descriptorPool = m_descriptorPool;
    frameAllocateInfo.descriptorSetCount = imageCount;
    frameAllocateInfo.pSetLayouts = frameLayouts.data();

    m_frameDescriptorSets.resize(imageCount);
    CheckVulkan(
        vkAllocateDescriptorSets(m_device, &frameAllocateInfo, m_frameDescriptorSets.data()),
        "Failed to allocate frame descriptor sets");

    // Set 1: one material descriptor set per swapchain image per material, allocated from the
    // material set layout.
    const uint32_t descriptorSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, m_materialSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = descriptorSetCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(descriptorSetCount);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate material descriptor sets");

    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CameraUniformData);

        // The camera uniform buffer is written once per image here, into the set 0 allocated for
        // that image — not once per material, which is what made the old single-set layout
        // wasteful and is the whole point of this split.
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowInfo.imageView = m_shadowMap.imageView;
        shadowInfo.sampler = m_shadowMap.sampler;

        VkDescriptorBufferInfo motionInfo{};
        motionInfo.buffer = m_motionBuffers[i];
        motionInfo.offset = 0;
        motionInfo.range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 3> frameWrites{};
        frameWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[0].dstSet = m_frameDescriptorSets[i];
        frameWrites[0].dstBinding = 0;
        frameWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        frameWrites[0].descriptorCount = 1;
        frameWrites[0].pBufferInfo = &bufferInfo;
        frameWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[1].dstSet = m_frameDescriptorSets[i];
        frameWrites[1].dstBinding = 1;
        frameWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        frameWrites[1].descriptorCount = 1;
        frameWrites[1].pImageInfo = &shadowInfo;
        frameWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[2].dstSet = m_frameDescriptorSets[i];
        frameWrites[2].dstBinding = 2;
        frameWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        frameWrites[2].descriptorCount = 1;
        frameWrites[2].pBufferInfo = &motionInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(frameWrites.size()), frameWrites.data(), 0, nullptr);

        for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(m_materialBindings.size()); ++materialIndex)
        {
            const MaterialTextureBinding& materialBinding = m_materialBindings[materialIndex];

            const size_t descriptorIndex =
                static_cast<size_t>(i) * m_materialBindings.size() + materialIndex;

            const std::array<TextureDescriptorBinding, 13> textureBindings = {
                materialBinding.baseColor,
                materialBinding.normal,
                materialBinding.metallic,
                materialBinding.roughness,
                materialBinding.occlusion,
                materialBinding.emissive,
                materialBinding.secondaryBaseColor,
                materialBinding.secondaryNormal,
                materialBinding.secondaryMetallic,
                materialBinding.secondaryRoughness,
                materialBinding.secondaryOcclusion,
                materialBinding.secondaryEmissive,
                materialBinding.blendMask};

            std::array<VkDescriptorImageInfo, 13> imageInfos{};
            for (size_t textureBindingIndex = 0; textureBindingIndex < textureBindings.size(); ++textureBindingIndex)
            {
                imageInfos[textureBindingIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[textureBindingIndex].imageView = textureBindings[textureBindingIndex].imageView;
                imageInfos[textureBindingIndex].sampler = textureBindings[textureBindingIndex].sampler;
            }

            std::array<VkWriteDescriptorSet, 13> descriptorWrites{};
            for (uint32_t bindingIndex = 0; bindingIndex < static_cast<uint32_t>(descriptorWrites.size()); ++bindingIndex)
            {
                descriptorWrites[bindingIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrites[bindingIndex].dstSet = m_descriptorSets[descriptorIndex];
                descriptorWrites[bindingIndex].dstBinding = bindingIndex;
                descriptorWrites[bindingIndex].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                descriptorWrites[bindingIndex].descriptorCount = 1;
                descriptorWrites[bindingIndex].pImageInfo = &imageInfos[bindingIndex];
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
}
