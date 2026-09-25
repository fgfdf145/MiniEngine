#include "uniform_buffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
    TextureDescriptorBinding localShadowAtlas,
    EnvironmentDescriptorBindings environment,
    std::span<const GpuMaterialData> drawMaterials,
    std::span<const GpuTextureTransforms> drawTextureTransforms)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_materialBindings(materialBindings),
      m_shadowMap(shadowMap),
      m_localShadowAtlas(localShadowAtlas),
      m_environment(environment),
      m_frameSetLayout(frameSetLayout),
      m_materialSetLayout(materialSetLayout),
      // A zero-sized storage buffer is invalid, and a scene with no submeshes still binds set 0.
      m_motionSlotCount(std::max(static_cast<uint32_t>(drawMaterials.size()), 1u)),
      // A zero-sized storage buffer is invalid, so a scene with no draws still gets one record.
      m_drawMaterials(drawMaterials.empty() ? std::vector<GpuMaterialData>(1) : std::vector<GpuMaterialData>(drawMaterials.begin(), drawMaterials.end())),
      m_drawTextureTransforms(
          drawTextureTransforms.empty() ? std::vector<GpuTextureTransforms>(1)
                                        : std::vector<GpuTextureTransforms>(drawTextureTransforms.begin(), drawTextureTransforms.end())),
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

    const auto destroyMapped = [this](std::vector<VkBuffer>& buffers, std::vector<VkDeviceMemory>& memories, std::vector<void*>& mapped)
    {
        for (size_t i = 0; i < buffers.size(); ++i)
        {
            if (buffers[i] != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, buffers[i], nullptr);
            }
            // Freeing mapped memory unmaps it implicitly.
            if (memories[i] != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, memories[i], nullptr);
            }
        }
        buffers.clear();
        memories.clear();
        mapped.clear();
    };
    destroyMapped(m_lightBuffers, m_lightMemories, m_mappedLightBuffers);
    if (m_materialBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_materialBuffer, nullptr);
        m_materialBuffer = VK_NULL_HANDLE;
    }
    if (m_materialMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_materialMemory, nullptr);
        m_materialMemory = VK_NULL_HANDLE;
    }
    m_mappedMaterialBuffer = nullptr;
    if (m_textureTransformBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, m_textureTransformBuffer, nullptr);
        m_textureTransformBuffer = VK_NULL_HANDLE;
    }
    if (m_textureTransformMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, m_textureTransformMemory, nullptr);
        m_textureTransformMemory = VK_NULL_HANDLE;
    }
    m_mappedTextureTransformBuffer = nullptr;
    destroyMapped(m_clusterBuffers, m_clusterMemories, m_mappedClusterBuffers);
    destroyMapped(m_shadowTileBuffers, m_shadowTileMemories, m_mappedShadowTileBuffers);

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

void VulkanUniformBuffer::SetEnvironmentMap(TextureDescriptorBinding environmentMap)
{
    m_environment.environmentMap = environmentMap;
    const VkDescriptorImageInfo info{environmentMap.sampler, environmentMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (VkDescriptorSet set : m_frameDescriptorSets)
    {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 6;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
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
    bool usesFallbackAmbient,
    const LightUpload& lights,
    const ShadowUniformData& shadow,
    const glm::mat4& prevViewProj,
    std::span<const glm::mat4> prevModels,
    const EnvironmentUniformData& environment,
    const glm::mat4& viewProjNoJitter,
    bool specularAntiAliasing,
    float preExposure)
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
    data.ambientLuminance = glm::vec4(ambientLuminance, usesFallbackAmbient ? 1.0f : 0.0f);

    // The shader indexes the light buffer with these counts and with the grid's indices, so all of
    // them are checked against what the buffers hold rather than trusted.
    const uint32_t lightCount = std::min(static_cast<uint32_t>(lights.lights.size()), kMaxSceneLights);
    const bool clustered = lights.clustered && lights.clusters != nullptr;
    data.lightCounts = glm::uvec4(std::min(lights.directionalCount, lightCount), lightCount, clustered ? 1u : 0u, 0u);
    std::memcpy(m_mappedLightBuffers[imageIndex], lights.lights.data(), sizeof(GpuLightData) * lightCount);
    // Every tile a light points at must be one uploaded here.
    const uint32_t tileCount = static_cast<uint32_t>(lights.shadowTiles.size());
    if (tileCount > kLocalShadowTileCount)
    {
        throw std::runtime_error("More local shadow tiles than the tile buffer holds");
    }
    for (uint32_t index = 0; index < lightCount; ++index)
    {
        const float tileRef = lights.lights[index].areaRightAxis.w;
        if (tileRef < 0.0f || tileRef > static_cast<float>(tileCount))
        {
            throw std::runtime_error("A light points past the uploaded local shadow tiles");
        }
    }
    std::memcpy(m_mappedShadowTileBuffers[imageIndex], lights.shadowTiles.data(), lights.shadowTiles.size_bytes());
    if (clustered)
    {
        const LightClusterGrid& grid = *lights.clusters;
        if (grid.ranges.size() != kLightClusterCount || grid.indices.size() > kLightClusterIndexCapacity)
        {
            throw std::runtime_error("Light cluster grid does not fit the cluster buffer");
        }
        for (uint32_t index : grid.indices)
        {
            if (index >= lightCount)
            {
                throw std::runtime_error("Light cluster grid indexes past the uploaded lights");
            }
        }
        data.lightClusterSlices = glm::vec4(grid.sliceScale, grid.sliceBias, 0.0f, 0.0f);
        auto* clusterBytes = static_cast<std::byte*>(m_mappedClusterBuffers[imageIndex]);
        std::memcpy(clusterBytes, grid.ranges.data(), sizeof(glm::uvec2) * kLightClusterCount);
        std::memcpy(clusterBytes + sizeof(glm::uvec2) * kLightClusterCount, grid.indices.data(), sizeof(uint32_t) * grid.indices.size());
    }
    data.shadow = shadow;
    data.prevViewProj = prevViewProj;
    data.environment = environment;
    data.viewProjNoJitter = viewProjNoJitter;
    data.specularAntiAliasing = glm::vec4(specularAntiAliasing ? 1.0f : 0.0f, kSpecularAAVariance, kSpecularAAThreshold, 0.0f);
    data.exposure = glm::vec4(preExposure, 1.0f / preExposure, 0.0f, 0.0f);

    std::memcpy(m_mappedBuffers[imageIndex], &data, sizeof(data));
    std::memcpy(m_mappedMotionBuffers[imageIndex], prevModels.data(), prevModels.size_bytes());
}

VulkanFrameDescriptorSetLayout::VulkanFrameDescriptorSetLayout(VkDevice device)
    : m_device(device)
{
    std::array<VkDescriptorSetLayoutBinding, 19> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // Compute too: the AO passes reconstruct view-space positions from the camera block.
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
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
    // The atmosphere LUTs (3 transmittance, 4 sky-view, 5 aerial perspective) and the HDRI (6),
    // sampled by the sky, lighting and forward fragment shaders, and by the compute shader that
    // projects the sky onto SH.
    for (uint32_t binding = 3; binding <= 6; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // The atmosphere's radiance SH, read by the shading of every surface.
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // The prefiltered sky (8) and the DFG table (9), for the specular lobe under a physical sky.
    for (uint32_t binding = 8; binding <= 9; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // The scene lights (10) and the cluster grid that indexes them (11), read by ShadeSurface.
    for (uint32_t binding = 10; binding <= 11; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // Every draw's material, read by the material fragment shaders at their draw slot.
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // The local shadow atlas (13) and its tiles (14), read by ShadeSurface.
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // The area lights' LTC tables.
    for (uint32_t binding = 15; binding <= 16; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // Each draw's texture transforms, read by gbuffer.frag and triangle.frag.
    bindings[17].binding = 17;
    bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[17].descriptorCount = 1;
    bindings[17].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // The transmission copy, sampled by triangle.frag for transmissive surfaces.
    bindings[18].binding = 18;
    bindings[18].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[18].descriptorCount = 1;
    bindings[18].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

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
    std::array<VkDescriptorSetLayoutBinding, kMaterialTextureBindingCount> bindings{};
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

    constexpr VkDeviceSize kLightBytes = sizeof(GpuLightData) * kMaxSceneLights;
    constexpr VkDeviceSize kClusterBytes =
        sizeof(glm::uvec2) * kLightClusterCount + sizeof(uint32_t) * kLightClusterIndexCapacity;
    m_lightBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_lightMemories.assign(imageCount, VK_NULL_HANDLE);
    m_mappedLightBuffers.assign(imageCount, nullptr);
    m_clusterBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_clusterMemories.assign(imageCount, VK_NULL_HANDLE);
    m_mappedClusterBuffers.assign(imageCount, nullptr);
    constexpr VkDeviceSize kShadowTileBytes = sizeof(GpuLocalShadowTile) * kLocalShadowTileCount;
    m_shadowTileBuffers.assign(imageCount, VK_NULL_HANDLE);
    m_shadowTileMemories.assign(imageCount, VK_NULL_HANDLE);
    m_mappedShadowTileBuffers.assign(imageCount, nullptr);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        CreateMappedBuffer(kShadowTileBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_shadowTileBuffers[i], m_shadowTileMemories[i], m_mappedShadowTileBuffers[i]);
        std::memset(m_mappedShadowTileBuffers[i], 0, static_cast<size_t>(kShadowTileBytes));
        CreateMappedBuffer(kLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_lightBuffers[i], m_lightMemories[i], m_mappedLightBuffers[i]);
        CreateMappedBuffer(kClusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_clusterBuffers[i], m_clusterMemories[i], m_mappedClusterBuffers[i]);
        // Empty until the first Update: no light, no cluster lists anything.
        std::memset(m_mappedLightBuffers[i], 0, static_cast<size_t>(kLightBytes));
        std::memset(m_mappedClusterBuffers[i], 0, static_cast<size_t>(kClusterBytes));
    }

    const VkDeviceSize materialBytes = sizeof(GpuMaterialData) * m_drawMaterials.size();
    CreateMappedBuffer(materialBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_materialBuffer, m_materialMemory, m_mappedMaterialBuffer);
    std::memcpy(m_mappedMaterialBuffer, m_drawMaterials.data(), static_cast<size_t>(materialBytes));

    if (m_drawTextureTransforms.size() != m_drawMaterials.size())
    {
        throw std::runtime_error("Every draw needs its texture transforms");
    }
    const VkDeviceSize transformBytes = sizeof(GpuTextureTransforms) * m_drawTextureTransforms.size();
    CreateMappedBuffer(transformBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_textureTransformBuffer, m_textureTransformMemory, m_mappedTextureTransformBuffer);
    std::memcpy(m_mappedTextureTransformBuffer, m_drawTextureTransforms.data(), static_cast<size_t>(transformBytes));
}

void VulkanUniformBuffer::CreateMappedBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    void*& mapped)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "Failed to create light buffer");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(m_device, buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &memory), "Failed to allocate light buffer memory");
    CheckVulkan(vkBindBufferMemory(m_device, buffer, memory, 0), "Failed to bind light buffer memory");
    CheckVulkan(vkMapMemory(m_device, memory, 0, size, 0, &mapped), "Failed to map light buffer memory");
}

void VulkanUniformBuffer::CreateDescriptorPool(uint32_t imageCount)
{
    // One pool serves both sets the split produced: imageCount uniform buffers, shadow map
    // samplers and six storage buffers (previous models, sky SH, lights, clusters, materials, shadow
    // tiles) for set 0 and thirteen samplers per material set for
    // set 1. That is why neither its name nor its failure message belongs to either half.
    const uint32_t materialSetCount = imageCount * static_cast<uint32_t>(m_materialBindings.size());
    const std::array<VkDescriptorPoolSize, 3> poolSizes = {{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
                                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, materialSetCount * kMaterialTextureBindingCount + imageCount * 11},
                                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount * 7}}};

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

        std::array<VkWriteDescriptorSet, 19> frameWrites{};
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
        const std::array<VkDescriptorImageInfo, 4> environmentInfos = {
            VkDescriptorImageInfo{m_environment.transmittance.sampler, m_environment.transmittance.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.skyView.sampler, m_environment.skyView.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.aerialPerspective.sampler, m_environment.aerialPerspective.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.environmentMap.sampler, m_environment.environmentMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t index = 0; index < 4; ++index)
        {
            VkWriteDescriptorSet& write = frameWrites[3 + index];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_frameDescriptorSets[i];
            write.dstBinding = 3 + index;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &environmentInfos[index];
        }
        const VkDescriptorBufferInfo irradianceInfo{m_environment.irradiance, 0, VK_WHOLE_SIZE};
        frameWrites[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[7].dstSet = m_frameDescriptorSets[i];
        frameWrites[7].dstBinding = 7;
        frameWrites[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        frameWrites[7].descriptorCount = 1;
        frameWrites[7].pBufferInfo = &irradianceInfo;
        const std::array<VkDescriptorImageInfo, 2> specularInfos = {
            VkDescriptorImageInfo{m_environment.prefiltered.sampler, m_environment.prefiltered.imageView, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{m_environment.brdfLut.sampler, m_environment.brdfLut.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t index = 0; index < 2; ++index)
        {
            VkWriteDescriptorSet& write = frameWrites[8 + index];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_frameDescriptorSets[i];
            write.dstBinding = 8 + index;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &specularInfos[index];
        }

        const std::array<VkDescriptorBufferInfo, 2> lightInfos = {
            VkDescriptorBufferInfo{m_lightBuffers[i], 0, VK_WHOLE_SIZE},
            VkDescriptorBufferInfo{m_clusterBuffers[i], 0, VK_WHOLE_SIZE}};
        for (uint32_t index = 0; index < 2; ++index)
        {
            VkWriteDescriptorSet& write = frameWrites[10 + index];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_frameDescriptorSets[i];
            write.dstBinding = 10 + index;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &lightInfos[index];
        }
        const VkDescriptorBufferInfo materialInfo{m_materialBuffer, 0, VK_WHOLE_SIZE};
        frameWrites[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[12].dstSet = m_frameDescriptorSets[i];
        frameWrites[12].dstBinding = 12;
        frameWrites[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        frameWrites[12].descriptorCount = 1;
        frameWrites[12].pBufferInfo = &materialInfo;
        const VkDescriptorImageInfo atlasInfo{m_localShadowAtlas.sampler, m_localShadowAtlas.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        frameWrites[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[13].dstSet = m_frameDescriptorSets[i];
        frameWrites[13].dstBinding = 13;
        frameWrites[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        frameWrites[13].descriptorCount = 1;
        frameWrites[13].pImageInfo = &atlasInfo;
        const VkDescriptorBufferInfo shadowTileInfo{m_shadowTileBuffers[i], 0, VK_WHOLE_SIZE};
        frameWrites[14].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[14].dstSet = m_frameDescriptorSets[i];
        frameWrites[14].dstBinding = 14;
        frameWrites[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        frameWrites[14].descriptorCount = 1;
        frameWrites[14].pBufferInfo = &shadowTileInfo;
        const std::array<VkDescriptorImageInfo, 2> ltcInfos = {
            VkDescriptorImageInfo{m_environment.ltcInverseMatrices.sampler, m_environment.ltcInverseMatrices.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            VkDescriptorImageInfo{m_environment.ltcAmplitudes.sampler, m_environment.ltcAmplitudes.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t index = 0; index < 2; ++index)
        {
            VkWriteDescriptorSet& write = frameWrites[15 + index];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_frameDescriptorSets[i];
            write.dstBinding = 15 + index;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &ltcInfos[index];
        }
        const VkDescriptorBufferInfo textureTransformInfo{m_textureTransformBuffer, 0, VK_WHOLE_SIZE};
        frameWrites[17].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[17].dstSet = m_frameDescriptorSets[i];
        frameWrites[17].dstBinding = 17;
        frameWrites[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        frameWrites[17].descriptorCount = 1;
        frameWrites[17].pBufferInfo = &textureTransformInfo;
        const VkDescriptorImageInfo transmissionInfo{m_environment.transmission.sampler, m_environment.transmission.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        frameWrites[18].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        frameWrites[18].dstSet = m_frameDescriptorSets[i];
        frameWrites[18].dstBinding = 18;
        frameWrites[18].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        frameWrites[18].descriptorCount = 1;
        frameWrites[18].pImageInfo = &transmissionInfo;

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(frameWrites.size()), frameWrites.data(), 0, nullptr);

        for (uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(m_materialBindings.size()); ++materialIndex)
        {
            const MaterialTextureBinding& materialBinding = m_materialBindings[materialIndex];

            const size_t descriptorIndex =
                static_cast<size_t>(i) * m_materialBindings.size() + materialIndex;

            const std::array<TextureDescriptorBinding, kMaterialTextureBindingCount> textureBindings = {
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
                materialBinding.blendMask,
                materialBinding.clearcoat,
                materialBinding.clearcoatRoughness,
                materialBinding.sheenColor,
                materialBinding.sheenRoughness,
                materialBinding.anisotropy,
                materialBinding.specular,
                materialBinding.specularColor,
                materialBinding.clearcoatNormal,
                materialBinding.iridescence,
                materialBinding.iridescenceThickness,
                materialBinding.transmission,
                materialBinding.thickness};

            std::array<VkDescriptorImageInfo, kMaterialTextureBindingCount> imageInfos{};
            for (size_t textureBindingIndex = 0; textureBindingIndex < textureBindings.size(); ++textureBindingIndex)
            {
                imageInfos[textureBindingIndex].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[textureBindingIndex].imageView = textureBindings[textureBindingIndex].imageView;
                imageInfos[textureBindingIndex].sampler = textureBindings[textureBindingIndex].sampler;
            }

            std::array<VkWriteDescriptorSet, kMaterialTextureBindingCount> descriptorWrites{};
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
