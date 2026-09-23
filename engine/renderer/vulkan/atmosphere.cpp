#include "atmosphere.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>

namespace me
{

namespace
{
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Must match the sizes in shaders/vulkan/atmosphere_common.glsl.
constexpr std::array<VkExtent3D, 4> kLutExtents = {
    VkExtent3D{256, 64, 1},
    VkExtent3D{32, 32, 1},
    VkExtent3D{192, 108, 1},
    VkExtent3D{32, 32, 32}};
constexpr std::array<const char*, 4> kShaderNames = {
    "atmosphere_transmittance.comp.spv",
    "atmosphere_multiscattering.comp.spv",
    "atmosphere_skyview.comp.spv",
    "atmosphere_aerial_perspective.comp.spv"};

uint32_t GroupCount(uint32_t size, uint32_t groupSize)
{
    return (size + groupSize - 1) / groupSize;
}

void GlobalBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

VulkanAtmosphere::VulkanAtmosphere(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        CreateImages();
        CreateDescriptors();
        CreatePipelines(pipelineCache, frameSetLayout);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanAtmosphere::~VulkanAtmosphere()
{
    DestroyHandles();
}

TextureDescriptorBinding VulkanAtmosphere::GetTransmittanceBinding() const
{
    return TextureDescriptorBinding{m_images[kTransmittance].view, m_sampler};
}

TextureDescriptorBinding VulkanAtmosphere::GetSkyViewBinding() const
{
    return TextureDescriptorBinding{m_images[kSkyView].view, m_sampler};
}

TextureDescriptorBinding VulkanAtmosphere::GetAerialPerspectiveBinding() const
{
    return TextureDescriptorBinding{m_images[kAerialPerspective].view, m_sampler};
}

void VulkanAtmosphere::Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, const AtmosphereParameters* parameters)
{
    if (!m_imagesInitialized)
    {
        std::array<VkImageMemoryBarrier, kLutCount> barriers{};
        for (size_t lut = 0; lut < kLutCount; ++lut)
        {
            VkImageMemoryBarrier& barrier = barriers[lut];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_images[lut].image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(barriers.size()),
            barriers.data());
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (const LutImage& image : m_images)
        {
            vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        m_imagesInitialized = true;
    }

    // The previous frame's fragment reads (and this frame's clear) before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (parameters != nullptr)
    {
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_descriptorSet};
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout,
            0,
            static_cast<uint32_t>(sets.size()),
            sets.data(),
            0,
            nullptr);

        if (!m_staticLutParameters.has_value() || !(*m_staticLutParameters == *parameters))
        {
            Dispatch(commandBuffer, kTransmittance, GroupCount(256, 8), GroupCount(64, 8), 1);
            GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            Dispatch(commandBuffer, kMultiScattering, 32, 32, 1);
            GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            m_staticLutParameters = *parameters;
        }
        Dispatch(commandBuffer, kSkyView, GroupCount(192, 8), GroupCount(108, 8), 1);
        Dispatch(commandBuffer, kAerialPerspective, GroupCount(32, 8), GroupCount(32, 8), 32);
    }

    // This frame's writes before its fragment shaders sample them.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

void VulkanAtmosphere::Dispatch(VkCommandBuffer commandBuffer, Lut lut, uint32_t x, uint32_t y, uint32_t z) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[lut]);
    vkCmdDispatch(commandBuffer, x, y, z);
}

void VulkanAtmosphere::CreateImages()
{
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        const VkExtent3D extent = kLutExtents[lut];
        const bool volume = extent.depth > 1;
        LutImage& image = m_images[lut];

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.extent = extent;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kLutFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &image.image), "Failed to create an atmosphere LUT");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, image.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &image.memory), "Failed to allocate an atmosphere LUT");
        CheckVulkan(vkBindImageMemory(m_device, image.image, image.memory, 0), "Failed to bind an atmosphere LUT");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLutFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create an atmosphere LUT view");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create the atmosphere sampler");
}

void VulkanAtmosphere::CreateDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create the atmosphere set layout");

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the atmosphere descriptor pool");

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_setLayout;
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, &m_descriptorSet), "Failed to allocate the atmosphere descriptor set");

    std::array<VkDescriptorImageInfo, 6> infos{};
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        infos[lut] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_images[lut].view, VK_IMAGE_LAYOUT_GENERAL};
    }
    infos[4] = VkDescriptorImageInfo{m_sampler, m_images[kTransmittance].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[5] = VkDescriptorImageInfo{m_sampler, m_images[kMultiScattering].view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = m_descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = bindings[binding].descriptorType;
        writes[binding].pImageInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanAtmosphere::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout)
{
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create the atmosphere pipeline layout");

    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        const VulkanShaderModule shader(m_device, EnginePaths::ShaderRoot() / kShaderNames[lut]);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader.GetHandle();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = m_pipelineLayout;
        CheckVulkan(
            vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, &m_pipelines[lut]),
            "Failed to create an atmosphere pipeline");
    }
}

uint32_t VulkanAtmosphere::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 && (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }
    throw std::runtime_error("Failed to find a memory type for the atmosphere LUTs");
}

void VulkanAtmosphere::DestroyHandles()
{
    for (VkPipeline& pipeline : m_pipelines)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    for (LutImage& image : m_images)
    {
        if (image.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
        if (image.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, image.image, nullptr);
        }
        if (image.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, image.memory, nullptr);
        }
        image = LutImage{};
    }
}
}
