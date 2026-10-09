#include "compute_pass_util.h"

#include "nvrhi_resources.h"
#include "pipeline.h"
#include "sampler_settings.h"

#include <engine/core/paths/engine_paths.h>

#include <algorithm>
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

nvrhi::SamplerHandle CreateClampSampler(nvrhi::IDevice* device, VkFilter filter)
{
    return CreateNvrhiSampler(device, BuildClampSamplerDesc(filter == VK_FILTER_LINEAR), "Failed to create a compute pass sampler");
}

VkDescriptorSetLayout CreateComputeSetLayout(VkDevice device, std::span<const VkDescriptorType> types, VkShaderStageFlags stages)
{
    return CreateComputeSetLayout(device, types, {}, stages);
}

VkDescriptorSetLayout CreateComputeSetLayout(
    VkDevice device, std::span<const VkDescriptorType> types, std::span<const uint32_t> samplerBindings, VkShaderStageFlags stages)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size() + samplerBindings.size());
    for (uint32_t binding = 0; binding < static_cast<uint32_t>(types.size()); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = types[binding];
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = stages;
    }
    for (size_t i = 0; i < samplerBindings.size(); ++i)
    {
        VkDescriptorSetLayoutBinding& binding = bindings[types.size() + i];
        binding.binding = kSplitSamplerBindingOffset + samplerBindings[i];
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = stages;
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
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, passSetLayout};
    CreateComputePipeline(device, pipelineCache, setLayouts, shaderName, pushConstantSize, pipelineLayout, pipeline);
}

void CreateComputePipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    std::span<const VkDescriptorSetLayout> setLayouts,
    const char* shaderName,
    uint32_t pushConstantSize,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = pushConstantSize;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = pushConstantSize > 0 ? 1u : 0u;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "Failed to create a compute pipeline layout");
    pipeline = CreateComputeShaderPipeline(device, pipelineCache, pipelineLayout, shaderName);
}

VkPipeline CreateComputeShaderPipeline(VkDevice device, VkPipelineCache pipelineCache, VkPipelineLayout pipelineLayout, const char* shaderName)
{
    const VulkanShaderModule computeShader(device, EnginePaths::ShaderRoot() / shaderName);
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShader.GetHandle();
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CheckVulkan(
        vkCreateComputePipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline),
        "Failed to create a compute pipeline");
    return pipeline;
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

VkDescriptorPool CreateImageDescriptorPool(
    VkDevice device, uint32_t setCount, uint32_t sampledPerSet, uint32_t storagePerSet, uint32_t samplersPerSet)
{
    std::vector<VkDescriptorPoolSize> poolSizes;
    const auto add = [&](VkDescriptorType type, uint32_t perSet) {
        if (perSet > 0)
        {
            poolSizes.push_back(VkDescriptorPoolSize{type, setCount * perSet});
        }
    };
    add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledPerSet);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storagePerSet);
    add(VK_DESCRIPTOR_TYPE_SAMPLER, samplersPerSet);

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

VkImageMemoryBarrier ColorImageTransition(
    VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return barrier;
}

void TransitionColorImages(
    VkCommandBuffer commandBuffer,
    std::span<const VkImage> images,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess)
{
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(images.size());
    for (VkImage image : images)
    {
        barriers.push_back(ColorImageTransition(image, oldLayout, newLayout, srcAccess, dstAccess));
    }
    vkCmdPipelineBarrier(
        commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
}

void BeginFrameImageWrites(VkCommandBuffer commandBuffer, std::span<const VkImage> images)
{
    TransitionColorImages(
        commandBuffer,
        images,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void EndFrameImageWrites(VkCommandBuffer commandBuffer, std::span<const VkImage> images, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess)
{
    TransitionColorImages(
        commandBuffer,
        images,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        srcStage,
        srcAccess,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

HistoryImagePair::~HistoryImagePair()
{
    Destroy();
}

void HistoryImagePair::Create(nvrhi::IDevice* nvrhiDevice, VkDevice device, VkExtent2D extent, VkFormat format, VkImageUsageFlags extraUsage)
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
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        history.texture = CreateNvrhiImage(nvrhiDevice, imageInfo, history.image, "Failed to create a history image");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = history.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(CreateNativeImageView(m_device, &viewInfo, nullptr, &history.view), "Failed to create a history image view");
    }
}

void HistoryImagePair::Destroy()
{
    for (Image& history : m_images)
    {
        if (history.view != VK_NULL_HANDLE)
        {
            DestroyNativeImageView(m_device, history.view, nullptr);
        }
        // The image and its memory go with NVRHI's texture.
        history = Image{};
    }
}

VkImage HistoryImagePair::GetImage(uint32_t index) const
{
    return m_images.at(index).image;
}

VkImageView HistoryImagePair::GetView(uint32_t index) const
{
    return m_images.at(index).view;
}

nvrhi::ITexture* HistoryImagePair::GetTexture(uint32_t index) const
{
    return m_images.at(index).texture;
}

namespace
{
struct MipDownsampleConstants
{
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t padding[2] = {};
};
}

MipDownsample::MipDownsample(nvrhi::IDevice* device)
    : m_device(device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 0;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::Sampler(kSplitSamplerBindingOffset),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(MipDownsampleConstants))};
    m_layout = CreateNvrhiBindingLayout(m_device, desc, "Failed to create the mip downsample binding layout");
    m_arrayPipeline = CreateNvrhiComputePipeline(m_device, "mip_downsample.comp.spv", {m_layout});
    m_pipeline2d = CreateNvrhiComputePipeline(m_device, "mip_downsample_2d.comp.spv", {m_layout});
    m_sampler = CreateClampSampler(m_device, VK_FILTER_LINEAR);
}

std::vector<nvrhi::BindingSetHandle> MipDownsample::CreateBindingSets(nvrhi::ITexture* texture, uint32_t firstLevel) const
{
    const nvrhi::TextureDesc& textureDesc = texture->getDesc();
    const nvrhi::TextureDimension dimension =
        textureDesc.dimension == nvrhi::TextureDimension::Texture2D ? nvrhi::TextureDimension::Texture2D : nvrhi::TextureDimension::Texture2DArray;
    std::vector<nvrhi::BindingSetHandle> sets;
    for (uint32_t level = firstLevel; level < textureDesc.mipLevels; ++level)
    {
        // Views of one level each, a cube's faces as six layers of an array.
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(
                0, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level - 1, 1, 0, textureDesc.arraySize), dimension),
            nvrhi::BindingSetItem::Texture_UAV(
                1, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level, 1, 0, textureDesc.arraySize), dimension),
            nvrhi::BindingSetItem::Sampler(kSplitSamplerBindingOffset, m_sampler),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(MipDownsampleConstants))};
        sets.push_back(CreateNvrhiBindingSet(m_device, desc, m_layout, "Failed to create a mip downsample binding set"));
    }
    return sets;
}

void MipDownsample::Record(
    nvrhi::ICommandList* commandList,
    nvrhi::ITexture* texture,
    uint32_t firstLevel,
    std::span<const nvrhi::BindingSetHandle> sets) const
{
    const nvrhi::TextureDesc& textureDesc = texture->getDesc();
    nvrhi::ComputeState state;
    state.pipeline = textureDesc.dimension == nvrhi::TextureDimension::Texture2D ? m_pipeline2d : m_arrayPipeline;
    for (uint32_t level = firstLevel; level < textureDesc.mipLevels; ++level)
    {
        commandList->setTextureState(texture, nvrhi::TextureSubresourceSet(level - 1, 1, 0, textureDesc.arraySize), nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(texture, nvrhi::TextureSubresourceSet(level, 1, 0, textureDesc.arraySize), nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        state.bindings = {sets[level - firstLevel]};
        commandList->setComputeState(state);
        MipDownsampleConstants constants{};
        constants.targetWidth = std::max(textureDesc.width >> level, 1u);
        constants.targetHeight = std::max(textureDesc.height >> level, 1u);
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(
            (constants.targetWidth + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
            (constants.targetHeight + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
            textureDesc.arraySize);
    }
}
}
