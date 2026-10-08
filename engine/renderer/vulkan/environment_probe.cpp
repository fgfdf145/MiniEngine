#include "environment_probe.h"

#include "compute_pass_util.h"
#include "nvrhi_resources.h"
#include "pipeline.h"
#include "sampler_settings.h"

#include <engine/core/paths/engine_paths.h>

#include <cmath>
#include <cstring>
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

// What decides the captured sky: the uniform block, with the camera reduced to what the capture
// sees of it. The HDRI ignores the camera; the atmosphere's sky-view depends on the altitude only
// (the planet is round, but a kilometre's walk turns it by a hundredth of a degree), kept to the
// metre. The height fog is left out: the capture is never fogged. The clouds are in it.
// How often, in the clouds' seconds, the capture takes the moving clouds afresh: at 10 m/s they
// move 20 m, a hundredth of a degree from 1.5 km below, between captures.
constexpr float kCloudCaptureSeconds = 2.0f;

EnvironmentUniformData CaptureKey(const EnvironmentUniformData& environment)
{
    EnvironmentUniformData key = environment;
    key.heightFogDensity = glm::vec4(0.0f);
    key.heightFogColor = glm::vec4(0.0f);
    key.heightFogParams = glm::vec4(0.0f);
    // The clouds' jitter index changes every frame; the capture does not jitter.
    key.cloudParams.w = 0.0f;
    // The clouds move every frame (CloudMotion): the capture follows them once per
    // kCloudCaptureSeconds of their clock, not every frame.
    key.cloudShapeMotion = glm::vec4(0.0f);
    key.cloudDetailMotion = glm::vec4(0.0f);
    key.cloudLife = glm::vec4(0.0f);
    key.cloudMotionStep = glm::vec4(0.0f, 0.0f, 0.0f, std::floor(environment.cloudMotionStep.w / kCloudCaptureSeconds));
    const bool hdri = static_cast<EnvironmentMode>(static_cast<uint32_t>(environment.sunDirectionAndMode.w)) == EnvironmentMode::Hdri;
    const float altitudeMeters = hdri ? 0.0f : std::round(glm::length(glm::vec3(environment.cameraPositionKm)) * 1000.0f);
    key.cameraPositionKm = glm::vec4(0.0f, altitudeMeters, 0.0f, 0.0f);
    return key;
}
}

VulkanEnvironmentProbe::VulkanEnvironmentProbe(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
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

        nvrhi::SamplerDesc samplerDesc = BuildClampSamplerDesc(true);
        samplerDesc.mipFilter = true;
        samplerDesc.maxLod = static_cast<float>(kRadianceMipCount);
        m_sampler = CreateNvrhiSampler(nvrhiDevice, samplerDesc, "Failed to create the environment probe sampler");

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
    return BindTexture(m_prefiltered.cubeView, m_prefiltered.texture, m_sampler);
}

void VulkanEnvironmentProbe::Invalidate()
{
    m_captured = false;
}

void VulkanEnvironmentProbe::Record(
    VkCommandBuffer commandBuffer,
    VkDescriptorSet frameDescriptorSet,
    bool physicalSky,
    const EnvironmentUniformData& environment)
{
    const bool initializing = !m_imagesInitialized;
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
        // Set 0 samples the prefiltered cube.
        EndFrameImageWrites(commandBuffer, std::span(&m_prefiltered.image, 1), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        m_imagesInitialized = true;
    }

    const EnvironmentUniformData key = CaptureKey(environment);
    const bool capture = physicalSky && !(m_captured && std::memcmp(&key, &m_capturedEnvironment, sizeof(key)) == 0);
    m_captured = physicalSky && (m_captured || capture);
    if (capture)
    {
        m_capturedEnvironment = key;
    }
    else if (!initializing)
    {
        // The cubes already hold this sky (or nothing samples them under None); the barrier after
        // the capture or clear that filled them made them visible to every later fragment shader.
        return;
    }

    // The previous frame's reads and this frame's clears before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    if (capture)
    {
        const std::array<VkDescriptorSet, 2> captureSets = {frameDescriptorSet, m_descriptorSets[0]};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_capturePipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, captureSets.data(), 0, nullptr);
        vkCmdDispatch(commandBuffer, GroupCount(kCubeSize), GroupCount(kCubeSize), 6);

        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        RecordMipChain(commandBuffer);
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        BeginFrameImageWrites(commandBuffer, std::span(&m_prefiltered.image, 1));
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
        EndFrameImageWrites(commandBuffer, std::span(&m_prefiltered.image, 1));
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
    cube.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, cube.image, "Failed to create an environment cube");

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
    const VkDescriptorImageInfo radianceInfo{NativeSampler(m_sampler), m_radiance.cubeView, VK_IMAGE_LAYOUT_GENERAL};
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
    m_sampler = nullptr;
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
        // The image and its memory go with the texture, released with the rest below.
        *cube = CubeImage{};
    }
}
}
