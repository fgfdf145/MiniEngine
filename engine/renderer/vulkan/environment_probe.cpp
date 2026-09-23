#include "environment_probe.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>

namespace me
{

namespace
{
constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match PrefilterConstants in shaders/vulkan/environment_prefilter.comp.
struct PrefilterConstants
{
    float roughness = 0.0f;
    uint32_t size = 0;
};

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

uint32_t GroupCount(uint32_t size)
{
    return (size + 7) / 8;
}
}

VulkanEnvironmentProbe::VulkanEnvironmentProbe(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        m_radiance = CreateCube(
            kRadianceMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_prefiltered = CreateCube(
            kPrefilterMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_radianceStorageView = CreateArrayView(m_radiance.image, 0);
        for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
        {
            m_prefilteredStorageViews[mip] = CreateArrayView(m_prefiltered.image, mip);
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.maxLod = static_cast<float>(kRadianceMipCount);
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create the environment probe sampler");

        CreateDescriptors();
        CreatePipelines(pipelineCache, frameSetLayout);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanEnvironmentProbe::~VulkanEnvironmentProbe()
{
    DestroyHandles();
}

TextureDescriptorBinding VulkanEnvironmentProbe::GetPrefilteredBinding() const
{
    return TextureDescriptorBinding{m_prefiltered.cubeView, m_sampler};
}

void VulkanEnvironmentProbe::Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, bool physicalSky)
{
    if (!m_imagesInitialized)
    {
        std::array<VkImageMemoryBarrier, 2> barriers{};
        const std::array<std::pair<VkImage, uint32_t>, 2> images = {
            std::pair{m_radiance.image, kRadianceMipCount},
            std::pair{m_prefiltered.image, kPrefilterMipCount}};
        for (size_t index = 0; index < barriers.size(); ++index)
        {
            VkImageMemoryBarrier& barrier = barriers[index];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index].first;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, images[index].second, 0, 6};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());
        const VkClearColorValue black{};
        for (const auto& [image, mipCount] : images)
        {
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 6};
            vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        m_imagesInitialized = true;
    }

    // The previous frame's reads and this frame's clears before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    if (physicalSky)
    {
        const std::array<VkDescriptorSet, 2> captureSets = {frameDescriptorSet, m_descriptorSets[0]};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_capturePipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, captureSets.data(), 0, nullptr);
        vkCmdDispatch(commandBuffer, GroupCount(kCubeSize), GroupCount(kCubeSize), 6);

        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        RecordMipChain(commandBuffer);
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline);
        for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
        {
            const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_descriptorSets[mip]};
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, sets.data(), 0, nullptr);
            PrefilterConstants constants{};
            constants.roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMipCount - 1);
            constants.size = kCubeSize >> mip;
            vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(commandBuffer, GroupCount(constants.size), GroupCount(constants.size), 6);
        }
    }

    // This frame's writes before its fragment shaders sample them.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

void VulkanEnvironmentProbe::RecordMipChain(VkCommandBuffer commandBuffer) const
{
    for (uint32_t mip = 1; mip < kRadianceMipCount; ++mip)
    {
        const int32_t sourceSize = static_cast<int32_t>(kCubeSize >> (mip - 1));
        const int32_t targetSize = static_cast<int32_t>(kCubeSize >> mip);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
        blit.srcOffsets[1] = {sourceSize, sourceSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
        blit.dstOffsets[1] = {targetSize, targetSize, 1};
        vkCmdBlitImage(
            commandBuffer, m_radiance.image, VK_IMAGE_LAYOUT_GENERAL, m_radiance.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        // Each level reads the one written just before it.
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
}

VulkanEnvironmentProbe::CubeImage VulkanEnvironmentProbe::CreateCube(uint32_t mipCount, VkImageUsageFlags usage) const
{
    CubeImage cube{};
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {kCubeSize, kCubeSize, 1};
    imageInfo.mipLevels = mipCount;
    imageInfo.arrayLayers = 6;
    imageInfo.format = kCubeFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &cube.image), "Failed to create an environment cube");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, cube.image, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &cube.memory), "Failed to allocate an environment cube");
    CheckVulkan(vkBindImageMemory(m_device, cube.image, cube.memory, 0), "Failed to bind an environment cube");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cube.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 6};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &cube.cubeView), "Failed to create an environment cube view");
    return cube;
}

VkImageView VulkanEnvironmentProbe::CreateArrayView(VkImage image, uint32_t mip) const
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView view = VK_NULL_HANDLE;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &view), "Failed to create an environment cube storage view");
    return view;
}

void VulkanEnvironmentProbe::CreateDescriptors()
{
    // 0 the radiance cube's mip 0 (capture output), 1 the radiance cube sampled, 2 one prefiltered
    // mip (prefilter output).
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create the environment probe set layout");

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kPrefilterMipCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPrefilterMipCount}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kPrefilterMipCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the environment probe descriptor pool");

    const std::array<VkDescriptorSetLayout, kPrefilterMipCount> layouts = {
        m_setLayout, m_setLayout, m_setLayout, m_setLayout, m_setLayout, m_setLayout};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = kPrefilterMipCount;
    allocateInfo.pSetLayouts = layouts.data();
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate the environment probe descriptor sets");

    const VkDescriptorImageInfo captureInfo{VK_NULL_HANDLE, m_radianceStorageView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo radianceInfo{m_sampler, m_radiance.cubeView, VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
    {
        const VkDescriptorImageInfo prefilteredInfo{VK_NULL_HANDLE, m_prefilteredStorageViews[mip], VK_IMAGE_LAYOUT_GENERAL};
        const std::array<const VkDescriptorImageInfo*, 3> infos = {&captureInfo, &radianceInfo, &prefilteredInfo};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_descriptorSets[mip];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = bindings[binding].descriptorType;
            writes[binding].pImageInfo = infos[binding];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanEnvironmentProbe::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(PrefilterConstants);
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create the environment probe pipeline layout");

    const std::array<std::pair<const char*, VkPipeline*>, 2> pipelines = {
        std::pair{"environment_capture.comp.spv", &m_capturePipeline},
        std::pair{"environment_prefilter.comp.spv", &m_prefilterPipeline}};
    for (const auto& [shaderName, pipeline] : pipelines)
    {
        const VulkanShaderModule shader(m_device, EnginePaths::ShaderRoot() / shaderName);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader.GetHandle();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = m_pipelineLayout;
        CheckVulkan(vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, pipeline), "Failed to create an environment probe pipeline");
    }
}

uint32_t VulkanEnvironmentProbe::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
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
    throw std::runtime_error("Failed to find a memory type for the environment probe");
}

void VulkanEnvironmentProbe::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_capturePipeline, &m_prefilterPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
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
        m_descriptorSets = {};
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
    for (VkImageView& view : m_prefilteredStorageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (m_radianceStorageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_radianceStorageView, nullptr);
        m_radianceStorageView = VK_NULL_HANDLE;
    }
    for (CubeImage* cube : {&m_radiance, &m_prefiltered})
    {
        if (cube->cubeView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, cube->cubeView, nullptr);
        }
        if (cube->image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, cube->image, nullptr);
        }
        if (cube->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, cube->memory, nullptr);
        }
        *cube = CubeImage{};
    }
}
}
