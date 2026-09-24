#include "compute_pass_util.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>

namespace me
{

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("Failed to find a memory type for a compute pass image");
}

VkSampler CreateClampSampler(VkDevice device, VkFilter filter)
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSampler sampler = VK_NULL_HANDLE;
    CheckVulkan(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "Failed to create a compute pass sampler");
    return sampler;
}

VkDescriptorSetLayout CreateComputeSetLayout(VkDevice device, std::span<const VkDescriptorType> types)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (uint32_t binding = 0; binding < static_cast<uint32_t>(types.size()); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = types[binding];
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    CheckVulkan(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "Failed to create a compute pass descriptor set layout");
    return layout;
}

void CreateComputePipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout passSetLayout,
    const char* shaderName,
    uint32_t pushConstantSize,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline)
{
    const VulkanShaderModule computeShader(device, EnginePaths::ShaderRoot() / shaderName);

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = pushConstantSize;

    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, passSetLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "Failed to create a compute pipeline layout");

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShader.GetHandle();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    CheckVulkan(
        vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline),
        "Failed to create a compute pipeline");
}

void DispatchCompute(
    VkCommandBuffer commandBuffer,
    VkPipeline pipeline,
    VkPipelineLayout pipelineLayout,
    VkDescriptorSet frameSet,
    VkDescriptorSet passSet,
    const void* constants,
    uint32_t constantSize,
    VkExtent2D extent)
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    const std::array<VkDescriptorSet, 2> sets = {frameSet, passSet};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout,
        0,
        static_cast<uint32_t>(sets.size()),
        sets.data(),
        0,
        nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, constantSize, constants);
    vkCmdDispatch(
        commandBuffer,
        (extent.width + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        (extent.height + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
        1);
}

VkDescriptorPool CreateImageDescriptorPool(VkDevice device, uint32_t setCount, uint32_t samplersPerSet, uint32_t storagePerSet)
{
    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * samplersPerSet},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setCount * storagePerSet}};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckVulkan(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "Failed to create a compute pass descriptor pool");
    return pool;
}

std::vector<VkDescriptorSet> AllocateDescriptorSets(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t count)
{
    CheckVulkan(vkResetDescriptorPool(device, pool, 0), "Failed to reset a compute pass descriptor pool");
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool;
    allocateInfo.descriptorSetCount = count;
    allocateInfo.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(count, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(device, &allocateInfo, sets.data()), "Failed to allocate compute pass descriptor sets");
    return sets;
}

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = info;
    return write;
}

HistoryImagePair::~HistoryImagePair()
{
    Destroy();
}

void HistoryImagePair::Create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VkFormat format)
{
    Destroy();
    m_device = device;
    for (Image& history : m_images)
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &history.image), "Failed to create a history image");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, history.image, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &history.memory), "Failed to allocate history image memory");
        CheckVulkan(vkBindImageMemory(m_device, history.image, history.memory, 0), "Failed to bind history image memory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = history.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &history.view), "Failed to create a history image view");
    }
}

void HistoryImagePair::Destroy()
{
    for (Image& history : m_images)
    {
        if (history.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, history.view, nullptr);
        }
        if (history.image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, history.image, nullptr);
        }
        if (history.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, history.memory, nullptr);
        }
        history = Image{};
    }
}

VkImageView HistoryImagePair::GetView(uint32_t index) const
{
    return m_images.at(index).view;
}

void HistoryImagePair::RecordBarrier(VkCommandBuffer commandBuffer, bool historyValid) const
{
    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (size_t index = 0; index < barriers.size(); ++index)
    {
        VkImageMemoryBarrier& barrier = barriers[index];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = historyValid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_images[index].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data());
}
}
