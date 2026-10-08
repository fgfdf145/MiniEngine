#include "atmosphere.h"

#include "command.h"
#include "compute_pass_util.h"
#include "nvrhi_resources.h"
#include "sampler_settings.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/renderer/volumetric_clouds.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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
constexpr std::array<const char*, 10> kShaderNames = {
    "atmosphere_transmittance.comp.spv",
    "atmosphere_multiscattering.comp.spv",
    "atmosphere_skyview.comp.spv",
    "atmosphere_aerial_perspective.comp.spv",
    "atmosphere_irradiance.comp.spv",
    "cloud_noise.comp.spv",
    "cloud_shadow.comp.spv",
    "cloud_weather.comp.spv",
    "cloud_march.comp.spv",
    "cloud_resolve.comp.spv"};
constexpr VkFormat kCloudTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The billow volumes and the plume map (top and slope, 16 bits: 8 would step the tops by 12 m).
constexpr std::array<VkFormat, 3> kCloudNoiseFormats = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_UNORM};
// Must match SHAPE_SIZE and DETAIL_SIZE in shaders/vulkan/cloud_noise.comp and WEATHER_SIZE in
// shaders/vulkan/cloud_weather.comp (a square, not a volume).
constexpr std::array<uint32_t, 3> kCloudNoiseSizes = {128, 128, kCloudWeatherSize};
// Must match CLOUD_SHADOW_MAP_SIZE in shaders/vulkan/cloud_shadow.glsl.
constexpr uint32_t kCloudShadowSize = kCloudShadowMapSize;
constexpr VkDeviceSize kIrradianceBytes = 9 * 4 * sizeof(float);

// The march's target: one texel per 2 x 2 block of the scene, rounded up.
VkExtent2D CloudMarchExtent(VkExtent2D sceneExtent)
{
    return VkExtent2D{std::max((sceneExtent.width + 1) / 2, 1u), std::max((sceneExtent.height + 1) / 2, 1u)};
}

// The set 1 bindings a view holds its own images in: the aerial perspective volume (storage), the
// march's samples and the resolved clouds (storage), and the clouds' history (sampled).
constexpr uint32_t kAerialPerspectiveBinding = 3;
constexpr uint32_t kCloudTargetBinding = 11;
constexpr uint32_t kCloudResolvedBinding = 12;
constexpr uint32_t kCloudHistoryBinding = 13;
constexpr uint32_t kAtmosphereBindingCount = 15;

VkDescriptorType AtmosphereBindingType(uint32_t binding)
{
    return (binding < 4 || (binding >= 7 && binding < 13)) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
           : binding == 6 || binding == 14                 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

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
    nvrhi::IDevice* nvrhiDevice,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    try
    {
        // The cloud noise tiles; everything else clamps. Both read the base level alone.
        nvrhi::SamplerDesc cloudSamplerDesc = BuildClampSamplerDesc(true);
        cloudSamplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
        m_cloudSampler = CreateNvrhiSampler(nvrhiDevice, cloudSamplerDesc, "Failed to create the cloud noise sampler");
        m_sampler = CreateNvrhiSampler(nvrhiDevice, BuildClampSamplerDesc(true), "Failed to create the atmosphere sampler");
        CreateImages();
        CreateDescriptors();
        // The set Record's shared passes bind, whose view images are 1 x 1 and never read.
        m_placeholderView = CreateView();
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
    return BindTexture(m_images[kTransmittance].view, m_images[kTransmittance].texture, m_sampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetSkyViewBinding() const
{
    return BindTexture(m_images[kSkyView].view, m_images[kSkyView].texture, m_sampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetMultiScatteringBinding() const
{
    return BindTexture(m_images[kMultiScattering].view, m_images[kMultiScattering].texture, m_sampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetAerialPerspectiveBinding(const View& view) const
{
    return BindTexture(view.m_aerialPerspective.view, view.m_aerialPerspective.texture, m_sampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetCloudShapeNoiseBinding() const
{
    return BindTexture(m_cloudNoise[kCloudShape].view, m_cloudNoise[kCloudShape].texture, m_cloudSampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetCloudDetailNoiseBinding() const
{
    return BindTexture(m_cloudNoise[kCloudDetail].view, m_cloudNoise[kCloudDetail].texture, m_cloudSampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetCloudShadowBinding() const
{
    return BindTexture(m_cloudShadow.view, m_cloudShadow.texture, m_sampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetCloudWeatherBinding() const
{
    return BindTexture(m_cloudNoise[kCloudWeather].view, m_cloudNoise[kCloudWeather].texture, m_cloudSampler);
}

TextureDescriptorBinding VulkanAtmosphere::GetCloudTargetBinding(const View& view) const
{
    return BindTexture(view.m_cloudResolved.view, view.m_cloudResolved.texture, m_sampler);
}

VulkanAtmosphere::View::View(VkDevice device)
    : m_device(device)
{
}

VulkanAtmosphere::View::~View()
{
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
    for (LutImage* image : {&m_aerialPerspective, &m_cloudTarget, &m_cloudResolved, &m_cloudHistory})
    {
        DestroyImage(m_device, *image);
    }
}

void VulkanAtmosphere::View::RestartHistory()
{
    m_cloudFrame = 0;
    m_cloudHistoryRestart = true;
}

void VulkanAtmosphere::DestroyImage(VkDevice device, LutImage& image)
{
    if (image.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, image.view, nullptr);
    }
    // The image and its memory go with the texture, released with the rest below.
    image = LutImage{};
}

std::unique_ptr<VulkanAtmosphere::View> VulkanAtmosphere::CreateView()
{
    std::unique_ptr<View> view(new View(m_device));
    CreateLutImage(view->m_aerialPerspective, kLutExtents[kAerialPerspective]);
    // Placeholders until the renderer names the scene's extent (EnsureCloudTarget), so the
    // descriptors are valid from the start.
    CreateCloudTargets(*view, VkExtent2D{1, 1});

    const std::array<VkDescriptorPoolSize, 3> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &view->m_descriptorPool), "Failed to create an atmosphere view's descriptor pool");
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = view->m_descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_setLayout;
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, &view->m_descriptorSet), "Failed to allocate an atmosphere view's descriptor set");
    WriteViewDescriptors(*view);
    return view;
}

bool VulkanAtmosphere::EnsureCloudTarget(View& view, VkExtent2D sceneExtent)
{
    const VkExtent2D extent{std::max(sceneExtent.width, 1u), std::max(sceneExtent.height, 1u)};
    if (extent.width == view.m_cloudSceneExtent.width && extent.height == view.m_cloudSceneExtent.height)
    {
        return false;
    }
    for (LutImage* image : {&view.m_cloudTarget, &view.m_cloudResolved, &view.m_cloudHistory})
    {
        DestroyImage(m_device, *image);
    }
    CreateCloudTargets(view, extent);
    WriteViewDescriptors(view);
    return true;
}

void VulkanAtmosphere::InitializeView(VkCommandBuffer commandBuffer, View& view)
{
    if (view.m_initialized)
    {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = view.m_aerialPerspective.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commandBuffer, view.m_aerialPerspective.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    // Set 0 samples it.
    EndFrameImageWrites(commandBuffer, std::span(&view.m_aerialPerspective.image, 1), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    view.m_initialized = true;
}

void VulkanAtmosphere::RecordView(
    VkCommandBuffer commandBuffer,
    View& view,
    VkDescriptorSet frameDescriptorSet,
    bool atmosphere,
    VulkanGpuTimer* timer)
{
    InitializeView(commandBuffer, view);
    if (atmosphere)
    {
        // The volume follows the view's frustum, so it is rebuilt every frame. The previous frame's
        // reads of it came before Record's opening barrier.
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, view.m_descriptorSet};
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        BeginFrameImageWrites(commandBuffer, std::span(&view.m_aerialPerspective.image, 1));
        Dispatch(commandBuffer, kAerialPerspective, GroupCount(32, 8), GroupCount(32, 8), 32);
        EndFrameImageWrites(commandBuffer, std::span(&view.m_aerialPerspective.image, 1));
    }
    RecordClouds(commandBuffer, view, frameDescriptorSet, timer);
}

void VulkanAtmosphere::RecordClouds(VkCommandBuffer commandBuffer, View& view, VkDescriptorSet frameDescriptorSet, VulkanGpuTimer* timer)
{
    const bool historyValid = !view.m_cloudTargetFresh && !view.m_cloudHistoryRestart;
    view.m_cloudHistoryRestart = false;
    if (view.m_cloudTargetFresh)
    {
        std::array<VkImageMemoryBarrier, 3> barriers{};
        const std::array<VkImage, 3> images = {view.m_cloudTarget.image, view.m_cloudResolved.image, view.m_cloudHistory.image};
        for (size_t index = 0; index < barriers.size(); ++index)
        {
            VkImageMemoryBarrier& barrier = barriers[index];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(barriers.size()),
            barriers.data());
        view.m_cloudTargetFresh = false;
    }
    else
    {
        // The previous frame's sky reads of the resolved clouds and its copy into the history
        // before this frame's writes and reads.
        GlobalBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        // The resolved clouds rest where set 0 samples them.
        BeginFrameImageWrites(commandBuffer, std::span(&view.m_cloudResolved.image, 1));
    }

    const VkExtent2D sceneExtent = view.m_cloudSceneExtent;
    // Must match CloudReconstruction in shaders/vulkan/cloud_reconstruction.glsl.
    const std::array<uint32_t, 4> constants = {
        view.m_cloudFrame++ & 3u, historyValid ? 1u : 0u, sceneExtent.width, sceneExtent.height};
    const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, view.m_descriptorSet};
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(
        commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(sizeof(constants)), constants.data());
    const VkExtent2D marchExtent = CloudMarchExtent(sceneExtent);
    Dispatch(commandBuffer, kCloudMarchPipeline, GroupCount(marchExtent.width, 8), GroupCount(marchExtent.height, 8), 1);
    if (timer != nullptr)
    {
        timer->Mark(commandBuffer, "CloudMarch");
    }
    GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    Dispatch(commandBuffer, kCloudResolvePipeline, GroupCount(sceneExtent.width, 8), GroupCount(sceneExtent.height, 8), 1);

    // The resolved clouds become next frame's history; the resolve's reads of the old one come
    // first.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = VkExtent3D{sceneExtent.width, sceneExtent.height, 1};
    vkCmdCopyImage(commandBuffer, view.m_cloudResolved.image, VK_IMAGE_LAYOUT_GENERAL, view.m_cloudHistory.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    EndFrameImageWrites(
        commandBuffer, std::span(&view.m_cloudResolved.image, 1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VulkanAtmosphere::CreateCloudImage(LutImage& image, VkExtent2D extent, VkImageUsageFlags usage, const char* name)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = VkExtent3D{extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = kCloudTargetFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, image.image, (std::string("Failed to create the ") + name).c_str());

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kCloudTargetFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), (std::string("Failed to create the view of the ") + name).c_str());
}

void VulkanAtmosphere::CreateCloudTargets(View& view, VkExtent2D sceneExtent)
{
    CreateCloudImage(view.m_cloudTarget, CloudMarchExtent(sceneExtent), VK_IMAGE_USAGE_STORAGE_BIT, "cloud march target");
    CreateCloudImage(
        view.m_cloudResolved,
        sceneExtent,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        "resolved clouds");
    CreateCloudImage(view.m_cloudHistory, sceneExtent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "cloud history");
    view.m_cloudSceneExtent = sceneExtent;
    view.m_cloudTargetFresh = true;
}

void VulkanAtmosphere::WriteViewDescriptors(const View& view)
{
    std::array<VkDescriptorImageInfo, kAtmosphereBindingCount> infos{};
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        infos[lut] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_images[lut].view, VK_IMAGE_LAYOUT_GENERAL};
    }
    infos[kAerialPerspectiveBinding] = VkDescriptorImageInfo{VK_NULL_HANDLE, view.m_aerialPerspective.view, VK_IMAGE_LAYOUT_GENERAL};
    // Set 0 samples the transmittance too, so it rests in SHADER_READ_ONLY_OPTIMAL between its writes.
    infos[4] = VkDescriptorImageInfo{NativeSampler(m_sampler), m_images[kTransmittance].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[5] = VkDescriptorImageInfo{NativeSampler(m_sampler), m_images[kMultiScattering].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[7] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_cloudNoise[kCloudShape].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[8] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_cloudNoise[kCloudDetail].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[9] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_cloudShadow.view, VK_IMAGE_LAYOUT_GENERAL};
    infos[10] = VkDescriptorImageInfo{VK_NULL_HANDLE, m_cloudNoise[kCloudWeather].view, VK_IMAGE_LAYOUT_GENERAL};
    infos[kCloudTargetBinding] = VkDescriptorImageInfo{VK_NULL_HANDLE, view.m_cloudTarget.view, VK_IMAGE_LAYOUT_GENERAL};
    infos[kCloudResolvedBinding] = VkDescriptorImageInfo{VK_NULL_HANDLE, view.m_cloudResolved.view, VK_IMAGE_LAYOUT_GENERAL};
    infos[kCloudHistoryBinding] = VkDescriptorImageInfo{NativeSampler(m_sampler), view.m_cloudHistory.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo irradianceInfo{m_irradianceBuffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo plumeInfo{m_plumeBuffer, 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, kAtmosphereBindingCount> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = view.m_descriptorSet;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = AtmosphereBindingType(binding);
        if (binding == 6 || binding == 14)
        {
            writes[binding].pBufferInfo = binding == 6 ? &irradianceInfo : &plumeInfo;
        }
        else
        {
            writes[binding].pImageInfo = &infos[binding];
        }
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

std::optional<glm::vec3> VulkanAtmosphere::GetSkyAverageRadiance(uint32_t frameSlot) const
{
    const Readback& readback = m_readbacks.at(frameSlot);
    if (!readback.written || readback.mapped == nullptr)
    {
        return std::nullopt;
    }
    // The L0 coefficient is the radiance's average over the sphere times Y00 = 0.282095, and
    // 1 / (4 pi Y00) = Y00.
    return glm::vec3(readback.mapped[0], readback.mapped[1], readback.mapped[2]) * 0.282095f;
}

nvrhi::IBuffer* VulkanAtmosphere::GetIrradianceBuffer() const
{
    return m_irradianceHandle;
}

void VulkanAtmosphere::Record(
    VkCommandBuffer commandBuffer,
    VkDescriptorSet frameDescriptorSet,
    const AtmosphereParameters* parameters,
    uint32_t frameSlot,
    const glm::vec4* cloudLife)
{
    if (!m_imagesInitialized)
    {
        // The LUTs (the aerial perspective volume is each view's), the noise and the shadow map.
        std::vector<VkImage> images;
        for (const LutImage& image : m_images)
        {
            if (image.image != VK_NULL_HANDLE)
            {
                images.push_back(image.image);
            }
        }
        for (const LutImage& image : m_cloudNoise)
        {
            images.push_back(image.image);
        }
        images.push_back(m_cloudShadow.image);
        std::vector<VkImageMemoryBarrier> barriers(images.size());
        for (size_t index = 0; index < barriers.size(); ++index)
        {
            VkImageMemoryBarrier& barrier = barriers[index];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index];
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
            if (image.image == VK_NULL_HANDLE)
            {
                continue;
            }
            vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        for (const LutImage& image : m_cloudNoise)
        {
            vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        // No clouds, no shadow: the map starts fully lit.
        const VkClearColorValue lit{{1.0f, 1.0f, 1.0f, 1.0f}};
        vkCmdClearColorImage(commandBuffer, m_cloudShadow.image, VK_IMAGE_LAYOUT_GENERAL, &lit, 1, &range);
        vkCmdFillBuffer(commandBuffer, m_irradianceBuffer, 0, VK_WHOLE_SIZE, 0);
        // The plume table, before the plume map is first built below.
        const VkBufferCopy plumes{0, 0, static_cast<VkDeviceSize>(kCloudPlumeTableSize) * sizeof(CloudPlumeCell)};
        vkCmdCopyBuffer(commandBuffer, m_plumeStaging, m_plumeBuffer, 1, &plumes);
        EndFrameImageWrites(commandBuffer, FrameSampledImages(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        InitializeView(commandBuffer, *m_placeholderView);
        m_imagesInitialized = true;
    }
    else if (m_plumeStaging != VK_NULL_HANDLE && ++m_plumeStagingAge > VulkanCommandContext::kMaxFramesInFlight)
    {
        // Recording this frame waited for the one that copied the table.
        DestroyPlumeStaging();
    }

    // The previous frame's fragment reads (and this frame's clear) before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (!m_cloudNoiseBuilt)
    {
        // The noise depends on nothing, so it is built once, whatever the mode; the barrier at the
        // end makes it visible to the sky and the probe.
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_placeholderView->m_descriptorSet};
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout,
            0,
            static_cast<uint32_t>(sets.size()),
            sets.data(),
            0,
            nullptr);
        const std::array<VkImage, kCloudNoiseCount> noise = {
            m_cloudNoise[kCloudShape].image, m_cloudNoise[kCloudDetail].image, m_cloudNoise[kCloudWeather].image};
        BeginFrameImageWrites(commandBuffer, noise);
        const uint32_t groups = GroupCount(kCloudNoiseSizes[kCloudShape], 4);
        Dispatch(commandBuffer, kCloudNoisePipeline, groups, groups, groups);
        const uint32_t weatherGroups = GroupCount(kCloudNoiseSizes[kCloudWeather], 8);
        Dispatch(commandBuffer, kCloudWeatherPipeline, weatherGroups, weatherGroups, 1);
        // The shadow map below reads the noise.
        EndFrameImageWrites(commandBuffer, noise);
        m_cloudNoiseBuilt = true;
        m_cloudWeatherLife = cloudLife != nullptr ? *cloudLife : glm::vec4(-1.0f);
    }
    else if (cloudLife != nullptr && *cloudLife != m_cloudWeatherLife)
    {
        // The plumes have moved on in their lives (cloud_weather.comp reads the phases from the
        // frame's uniforms); the barrier above ordered last frame's reads of the map before this.
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_placeholderView->m_descriptorSet};
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            m_pipelineLayout,
            0,
            static_cast<uint32_t>(sets.size()),
            sets.data(),
            0,
            nullptr);
        BeginFrameImageWrites(commandBuffer, std::span(&m_cloudNoise[kCloudWeather].image, 1));
        const uint32_t weatherGroups = GroupCount(kCloudNoiseSizes[kCloudWeather], 8);
        Dispatch(commandBuffer, kCloudWeatherPipeline, weatherGroups, weatherGroups, 1);
        EndFrameImageWrites(commandBuffer, std::span(&m_cloudNoise[kCloudWeather].image, 1));
        m_cloudWeatherLife = *cloudLife;
    }

    if (parameters != nullptr)
    {
        const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_placeholderView->m_descriptorSet};
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
            BeginFrameImageWrites(commandBuffer, std::span(&m_images[kTransmittance].image, 1));
            Dispatch(commandBuffer, kTransmittance, GroupCount(256, 8), GroupCount(64, 8), 1);
            EndFrameImageWrites(commandBuffer, std::span(&m_images[kTransmittance].image, 1));
            Dispatch(commandBuffer, kMultiScattering, 32, 32, 1);
            GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            m_staticLutParameters = *parameters;
        }
        const std::array<VkImage, 2> skyAndShadow = {m_images[kSkyView].image, m_cloudShadow.image};
        BeginFrameImageWrites(commandBuffer, skyAndShadow);
        Dispatch(commandBuffer, kSkyView, GroupCount(192, 8), GroupCount(108, 8), 1);
        // Every frame, as the map follows the camera; all ones when the clouds are off.
        Dispatch(commandBuffer, kCloudShadowPipeline, GroupCount(kCloudShadowSize, 8), GroupCount(kCloudShadowSize, 8), 1);
        // The SH projection samples the sky-view LUT written above.
        EndFrameImageWrites(commandBuffer, skyAndShadow);
        Dispatch(commandBuffer, kIrradiancePipeline, 1, 1, 1);

        // A copy for the CPU, read once this slot's fence has signaled.
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        Readback& readback = m_readbacks.at(frameSlot);
        const VkBufferCopy copy{0, 0, kIrradianceBytes};
        vkCmdCopyBuffer(commandBuffer, m_irradianceBuffer, readback.buffer, 1, &copy);
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    m_readbacks.at(frameSlot).written = parameters != nullptr;

    // This frame's writes before its fragment shaders sample them.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

std::array<VkImage, 6> VulkanAtmosphere::FrameSampledImages() const
{
    return {
        m_images[kTransmittance].image,
        m_images[kSkyView].image,
        m_cloudNoise[kCloudShape].image,
        m_cloudNoise[kCloudDetail].image,
        m_cloudNoise[kCloudWeather].image,
        m_cloudShadow.image};
}

void VulkanAtmosphere::Dispatch(VkCommandBuffer commandBuffer, size_t pipeline, uint32_t x, uint32_t y, uint32_t z) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[pipeline]);
    vkCmdDispatch(commandBuffer, x, y, z);
}

void VulkanAtmosphere::CreateLutImage(LutImage& image, VkExtent3D extent)
{
    const bool volume = extent.depth > 1;
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
    image.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, image.image, "Failed to create an atmosphere LUT");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image.image;
    viewInfo.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kLutFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create an atmosphere LUT view");
}

void VulkanAtmosphere::CreateImages()
{
    // The aerial perspective volume is each view's (CreateView).
    for (size_t lut = 0; lut < kLutCount; ++lut)
    {
        if (lut != kAerialPerspective)
        {
            CreateLutImage(m_images[lut], kLutExtents[lut]);
        }
    }

    for (size_t noise = 0; noise < kCloudNoiseCount; ++noise)
    {
        const uint32_t size = kCloudNoiseSizes[noise];
        const bool volume = noise != kCloudWeather;
        LutImage& image = m_cloudNoise[noise];

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.extent = VkExtent3D{size, size, volume ? size : 1u};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kCloudNoiseFormats[noise];
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, image.image, "Failed to create a cloud noise volume");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kCloudNoiseFormats[noise];
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view), "Failed to create a cloud noise view");
    }

    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = VkExtent3D{kCloudShadowSize, kCloudShadowSize, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = kLutFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        m_cloudShadow.texture = CreateNvrhiImage(m_nvrhiDevice, imageInfo, m_cloudShadow.image, "Failed to create the cloud shadow map");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_cloudShadow.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLutFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_cloudShadow.view), "Failed to create the cloud shadow map view");
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = kIrradianceBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    m_irradianceHandle = CreateNvrhiBuffer(m_nvrhiDevice, bufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_irradianceBuffer, "Failed to create the sky irradiance buffer");

    // The plume table: drawn once on the CPU, staged, copied by the first Record.
    {
        const std::vector<CloudPlumeCell> table = BuildCloudPlumeTable();
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(table.size() * sizeof(CloudPlumeCell));
        const auto createBuffer = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, const char* name, void** mapped)
        {
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = bytes;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            return CreateNvrhiBuffer(m_nvrhiDevice, info, properties, buffer, (std::string("Failed to create the ") + name).c_str(), mapped);
        };
        m_plumeHandle = createBuffer(
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_plumeBuffer,
            "cloud plume table",
            nullptr);
        void* mapped = nullptr;
        m_plumeStagingHandle = createBuffer(
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_plumeStaging,
            "cloud plume staging buffer",
            &mapped);
        std::memcpy(mapped, table.data(), static_cast<size_t>(bytes));
        m_nvrhiDevice->unmapBuffer(m_plumeStagingHandle);
    }

    m_readbacks.resize(VulkanCommandContext::kMaxFramesInFlight);
    for (Readback& readback : m_readbacks)
    {
        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = kIrradianceBytes;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        void* mapped = nullptr;
        readback.handle = CreateNvrhiBuffer(
            m_nvrhiDevice,
            readbackInfo,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            readback.buffer,
            "Failed to create a sky readback buffer",
            &mapped);
        readback.mapped = static_cast<const float*>(mapped);
    }
}

void VulkanAtmosphere::DestroyPlumeStaging()
{
    // The buffer and its memory go with the handle.
    m_plumeStagingHandle = nullptr;
    m_plumeStaging = VK_NULL_HANDLE;
}

void VulkanAtmosphere::CreateDescriptors()
{
    // 0-3 the LUTs as storage images, 4-5 the transmittance and multiple-scattering LUTs sampled,
    // 6 the SH buffer, 7-8 the clouds' billow volumes, 9 their shadow map, 10 their plume map, 11
    // the march's samples and 12 the resolved clouds as storage images, 13 the clouds' history
    // sampled, and 14 their plume table.
    std::array<VkDescriptorSetLayoutBinding, kAtmosphereBindingCount> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = AtmosphereBindingType(binding);
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create the atmosphere set layout");

}

void VulkanAtmosphere::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout)
{
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    // The cloud passes' frame constants (cloud_reconstruction.glsl); the other passes ignore them.
    const VkPushConstantRange pushConstants{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(uint32_t)};
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create the atmosphere pipeline layout");

    for (size_t pipeline = 0; pipeline < kPipelineCount; ++pipeline)
    {
        const VulkanShaderModule shader(m_device, EnginePaths::ShaderRoot() / kShaderNames[pipeline]);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader.GetHandle();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = m_pipelineLayout;
        CheckVulkan(
            vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, &m_pipelines[pipeline]),
            "Failed to create an atmosphere pipeline");
    }
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
    m_placeholderView.reset();
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    m_sampler = nullptr;
    m_cloudSampler = nullptr;
    // The buffers and their memory (unmapped as it is freed) go with the handles.
    m_readbacks.clear();
    m_irradianceHandle = nullptr;
    m_irradianceBuffer = VK_NULL_HANDLE;
    DestroyPlumeStaging();
    m_plumeHandle = nullptr;
    m_plumeBuffer = VK_NULL_HANDLE;
    if (m_cloudShadow.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_cloudShadow.view, nullptr);
    }
    // The image and its memory go with the texture, released with the rest below.
    m_cloudShadow = LutImage{};
    for (LutImage& image : m_cloudNoise)
    {
        if (image.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
        // The image and its memory go with the texture, released with the rest below.
        image = LutImage{};
    }
    for (LutImage& image : m_images)
    {
        if (image.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, image.view, nullptr);
        }
        // The image and its memory go with the texture, released with the rest below.
        image = LutImage{};
    }
}
}
