#include "atmosphere.h"

#include "command.h"
#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "nvrhi_resources.h"
#include "sampler_settings.h"
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
// Must match the sizes in shaders/vulkan/atmosphere_common.slang.
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
// Must match CLOUD_SHADOW_MAP_SIZE in shaders/vulkan/cloud_shadow.slang.
constexpr uint32_t kCloudShadowSize = kCloudShadowMapSize;
constexpr VkDeviceSize kIrradianceBytes = 9 * 4 * sizeof(float);

// The march's target: one texel per 2 x 2 block of the scene, rounded up.
VkExtent2D CloudMarchExtent(VkExtent2D sceneExtent)
{
    return VkExtent2D{std::max((sceneExtent.width + 1) / 2, 1u), std::max((sceneExtent.height + 1) / 2, 1u)};
}

// Set 1, an NVRHI binding layout (VulkanAtmosphere::CreateBindingLayout): 0-3 the LUTs as storage
// images, 4-5 the transmittance and multiple-scattering LUTs sampled, 6 the SH buffer, 7-8 the
// clouds' billow volumes, 9 their shadow map, 10 their plume map, 11 the march's samples and 12 the
// resolved clouds as storage images, 13 the clouds' history sampled, 14 their plume table; each
// sampled texture's sampler at its binding + 64. A view's own images: the aerial perspective volume
// (3) and the clouds (11 to 13).
constexpr uint32_t kSamplerBindingOffset = 64;
constexpr uint32_t kAerialPerspectiveBinding = 3;
constexpr uint32_t kTransmittanceSampledBinding = 4;
constexpr uint32_t kMultiScatteringSampledBinding = 5;
constexpr uint32_t kIrradianceBinding = 6;
constexpr uint32_t kCloudTargetBinding = 11;
constexpr uint32_t kCloudResolvedBinding = 12;
constexpr uint32_t kCloudHistoryBinding = 13;
constexpr uint32_t kPlumeBinding = 14;
// The cloud passes' frame constants (cloud_reconstruction.slang); the other passes ignore them.
constexpr uint32_t kPushConstantBytes = 4 * sizeof(uint32_t);

uint32_t GroupCount(uint32_t size, uint32_t groupSize)
{
    return (size + groupSize - 1) / groupSize;
}

}

VulkanAtmosphere::VulkanAtmosphere(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    nvrhi::IBindingLayout* frameSetLayout)
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
        CreateBindingLayout();
        // The set Record's shared passes bind, whose view images are 1 x 1 and never read.
        m_placeholderView = CreateView();
        CreatePipelines(frameSetLayout);
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
    m_bindingSet = nullptr;
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
    // binding set is valid from the start.
    CreateCloudTargets(*view, VkExtent2D{1, 1});
    CreateViewBindingSet(*view);
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
    view.m_bindingSet = nullptr;
    CreateCloudTargets(view, extent);
    CreateViewBindingSet(view);
    return true;
}

void VulkanAtmosphere::InitializeView(nvrhi::ICommandList* commandList, View& view)
{
    if (view.m_initialized)
    {
        return;
    }
    // Inside a scope that brings the volume out of UNDEFINED (Common) and leaves it where set 0
    // samples it.
    nvrhi::ITexture* volume = view.m_aerialPerspective.texture;
    commandList->setTextureState(volume, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->clearTextureFloat(volume, nvrhi::AllSubresources, nvrhi::Color(0.0f));
    commandList->setTextureState(volume, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    view.m_initialized = true;
}

void VulkanAtmosphere::RecordView(
    VkCommandBuffer commandBuffer,
    View& view,
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameBindingSet,
    bool atmosphere,
    VulkanGpuTimer* timer)
{
    // The view's images rest where the native passes and set 0 read them: the aerial perspective
    // volume and the resolved clouds in SHADER_READ_ONLY_OPTIMAL, the march's samples in GENERAL, the
    // history (read here alone) as a shader resource; new ones come out of UNDEFINED. The shared
    // multiple-scattering LUT, which the volume samples, rests in GENERAL (Record).
    using States = nvrhi::ResourceStates;
    const bool fresh = view.m_cloudTargetFresh;
    nvrhi::ITexture* multiScattering = m_images[kMultiScattering].texture;
    const NvrhiPassScope scope(
        commandList,
        {{multiScattering, States::UnorderedAccess},
         {view.m_aerialPerspective.texture, view.m_initialized ? States::ShaderResource : States::Common, States::ShaderResource},
         {view.m_cloudTarget.texture, fresh ? States::Common : States::UnorderedAccess, States::UnorderedAccess},
         {view.m_cloudResolved.texture, fresh ? States::Common : States::ShaderResource, States::ShaderResource},
         {view.m_cloudHistory.texture, fresh ? States::Common : States::ShaderResource, States::ShaderResource}});
    InitializeView(commandList, view);
    if (atmosphere)
    {
        // The volume follows the view's frustum, so it is rebuilt every frame.
        commandList->setTextureState(view.m_aerialPerspective.texture, nvrhi::AllSubresources, States::UnorderedAccess);
        commandList->setTextureState(multiScattering, nvrhi::AllSubresources, States::ShaderResource);
        commandList->commitBarriers();
        Dispatch(commandList, kAerialPerspective, frameBindingSet, view.m_bindingSet, GroupCount(32, 8), GroupCount(32, 8), 32);
        commandList->setTextureState(view.m_aerialPerspective.texture, nvrhi::AllSubresources, States::ShaderResource);
    }
    RecordClouds(commandBuffer, view, commandList, frameBindingSet, timer);
}

void VulkanAtmosphere::RecordClouds(
    VkCommandBuffer commandBuffer,
    View& view,
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameBindingSet,
    VulkanGpuTimer* timer)
{
    using States = nvrhi::ResourceStates;
    const bool historyValid = !view.m_cloudTargetFresh && !view.m_cloudHistoryRestart;
    view.m_cloudHistoryRestart = false;
    view.m_cloudTargetFresh = false;

    const VkExtent2D sceneExtent = view.m_cloudSceneExtent;
    // Must match CloudReconstruction in shaders/vulkan/cloud_reconstruction.slang.
    const std::array<uint32_t, 4> constants = {
        view.m_cloudFrame++ & 3u, historyValid ? 1u : 0u, sceneExtent.width, sceneExtent.height};
    nvrhi::ITexture* samples = view.m_cloudTarget.texture;
    nvrhi::ITexture* resolved = view.m_cloudResolved.texture;
    nvrhi::ITexture* history = view.m_cloudHistory.texture;
    // The previous frame's reads of each image come before this frame's writes (the states' barriers).
    commandList->setTextureState(samples, nvrhi::AllSubresources, States::UnorderedAccess);
    commandList->commitBarriers();
    const VkExtent2D marchExtent = CloudMarchExtent(sceneExtent);
    Dispatch(
        commandList,
        kCloudMarchPipeline,
        frameBindingSet,
        view.m_bindingSet,
        GroupCount(marchExtent.width, 8),
        GroupCount(marchExtent.height, 8),
        1,
        constants.data());
    if (timer != nullptr)
    {
        timer->Mark(commandBuffer, "CloudMarch");
    }
    // The resolve reads the samples as storage too: the second UnorderedAccess is NVRHI's UAV barrier.
    commandList->setTextureState(samples, nvrhi::AllSubresources, States::UnorderedAccess);
    commandList->setTextureState(resolved, nvrhi::AllSubresources, States::UnorderedAccess);
    commandList->setTextureState(history, nvrhi::AllSubresources, States::ShaderResource);
    commandList->commitBarriers();
    Dispatch(
        commandList,
        kCloudResolvePipeline,
        frameBindingSet,
        view.m_bindingSet,
        GroupCount(sceneExtent.width, 8),
        GroupCount(sceneExtent.height, 8),
        1,
        constants.data());

    // The resolved clouds become next frame's history; the resolve's reads of the old one come first.
    commandList->setTextureState(resolved, nvrhi::AllSubresources, States::CopySource);
    commandList->setTextureState(history, nvrhi::AllSubresources, States::CopyDest);
    commandList->commitBarriers();
    commandList->copyTexture(history, nvrhi::TextureSlice(), resolved, nvrhi::TextureSlice());
    // The scope leaves both where the sky pass and next frame's resolve read them.
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

void VulkanAtmosphere::CreateViewBindingSet(View& view)
{
    using Item = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc desc;
    desc.bindings = {
        Item::Texture_UAV(kTransmittance, m_images[kTransmittance].texture),
        Item::Texture_UAV(kMultiScattering, m_images[kMultiScattering].texture),
        Item::Texture_UAV(kSkyView, m_images[kSkyView].texture),
        Item::Texture_UAV(kAerialPerspectiveBinding, view.m_aerialPerspective.texture),
        Item::Texture_SRV(kTransmittanceSampledBinding, m_images[kTransmittance].texture),
        Item::Sampler(kTransmittanceSampledBinding + kSamplerBindingOffset, m_sampler),
        Item::Texture_SRV(kMultiScatteringSampledBinding, m_images[kMultiScattering].texture),
        Item::Sampler(kMultiScatteringSampledBinding + kSamplerBindingOffset, m_sampler),
        Item::RawBuffer_UAV(kIrradianceBinding, m_irradianceHandle),
        Item::Texture_UAV(7, m_cloudNoise[kCloudShape].texture),
        Item::Texture_UAV(8, m_cloudNoise[kCloudDetail].texture),
        Item::Texture_UAV(9, m_cloudShadow.texture),
        Item::Texture_UAV(10, m_cloudNoise[kCloudWeather].texture),
        Item::Texture_UAV(kCloudTargetBinding, view.m_cloudTarget.texture),
        Item::Texture_UAV(kCloudResolvedBinding, view.m_cloudResolved.texture),
        Item::Texture_SRV(kCloudHistoryBinding, view.m_cloudHistory.texture),
        Item::Sampler(kCloudHistoryBinding + kSamplerBindingOffset, m_sampler),
        Item::RawBuffer_SRV(kPlumeBinding, m_plumeHandle),
        Item::PushConstants(0, kPushConstantBytes)};
    view.m_bindingSet = CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an atmosphere view's binding set");
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
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameBindingSet,
    const AtmosphereParameters* parameters,
    uint32_t frameSlot,
    const glm::vec4* cloudLife)
{
    // What set 0 samples rests in SHADER_READ_ONLY_OPTIMAL, the multiple-scattering LUT (which the
    // path tracer reads through its own set) in GENERAL, the SH buffer as a shader resource; before
    // the first Record everything is UNDEFINED. The states set below order each write after the
    // previous frame's reads, and the scope's closing ones each read after this frame's writes.
    using States = nvrhi::ResourceStates;
    const bool initializing = !m_imagesInitialized;
    const States sampled = initializing ? States::Common : States::ShaderResource;
    nvrhi::ITexture* transmittance = m_images[kTransmittance].texture;
    nvrhi::ITexture* multiScattering = m_images[kMultiScattering].texture;
    nvrhi::ITexture* skyView = m_images[kSkyView].texture;
    nvrhi::ITexture* shape = m_cloudNoise[kCloudShape].texture;
    nvrhi::ITexture* detail = m_cloudNoise[kCloudDetail].texture;
    nvrhi::ITexture* weather = m_cloudNoise[kCloudWeather].texture;
    nvrhi::ITexture* shadow = m_cloudShadow.texture;
    nvrhi::ITexture* placeholderVolume = m_placeholderView->m_aerialPerspective.texture;
    Readback& readback = m_readbacks.at(frameSlot);
    const NvrhiPassScope scope(
        commandList,
        {{transmittance, sampled, States::ShaderResource},
         {multiScattering, initializing ? States::Common : States::UnorderedAccess, States::UnorderedAccess},
         {skyView, sampled, States::ShaderResource},
         {shape, sampled, States::ShaderResource},
         {detail, sampled, States::ShaderResource},
         {weather, sampled, States::ShaderResource},
         {shadow, sampled, States::ShaderResource},
         {placeholderVolume, m_placeholderView->m_initialized ? States::ShaderResource : States::Common, States::ShaderResource}},
        {{m_irradianceHandle, initializing ? States::Common : States::ShaderResource, States::ShaderResource},
         {m_plumeHandle, initializing ? States::Common : States::ShaderResource, States::ShaderResource},
         {readback.handle, States::CopyDest}});
    if (initializing)
    {
        // The LUTs (the aerial perspective volume is each view's), the noise and the shadow map.
        std::vector<nvrhi::ITexture*> images;
        for (const LutImage& image : m_images)
        {
            if (image.texture)
            {
                images.push_back(image.texture);
            }
        }
        for (const LutImage& image : m_cloudNoise)
        {
            images.push_back(image.texture);
        }
        for (nvrhi::ITexture* image : images)
        {
            commandList->setTextureState(image, nvrhi::AllSubresources, States::CopyDest);
        }
        commandList->setTextureState(shadow, nvrhi::AllSubresources, States::CopyDest);
        commandList->setBufferState(m_irradianceHandle, States::CopyDest);
        commandList->setBufferState(m_plumeHandle, States::CopyDest);
        commandList->setBufferState(m_plumeStagingHandle, States::CopySource);
        commandList->commitBarriers();
        for (nvrhi::ITexture* image : images)
        {
            commandList->clearTextureFloat(image, nvrhi::AllSubresources, nvrhi::Color(0.0f));
        }
        // No clouds, no shadow: the map starts fully lit.
        commandList->clearTextureFloat(shadow, nvrhi::AllSubresources, nvrhi::Color(1.0f));
        commandList->clearBufferUInt(m_irradianceHandle, 0);
        // The plume table, before the plume map is first built below.
        commandList->copyBuffer(m_plumeHandle, 0, m_plumeStagingHandle, 0, static_cast<uint64_t>(kCloudPlumeTableSize) * sizeof(CloudPlumeCell));
        for (nvrhi::ITexture* image : images)
        {
            commandList->setTextureState(image, nvrhi::AllSubresources, image == multiScattering ? States::UnorderedAccess : States::ShaderResource);
        }
        commandList->setTextureState(shadow, nvrhi::AllSubresources, States::ShaderResource);
        commandList->setBufferState(m_irradianceHandle, States::ShaderResource);
        commandList->setBufferState(m_plumeHandle, States::ShaderResource);
        InitializeView(commandList, *m_placeholderView);
        m_imagesInitialized = true;
    }
    else if (m_plumeStagingHandle && ++m_plumeStagingAge > VulkanCommandContext::kMaxFramesInFlight)
    {
        // Recording this frame waited for the one that copied the table.
        DestroyPlumeStaging();
    }

    nvrhi::IBindingSet* set = m_placeholderView->m_bindingSet;
    if (!m_cloudNoiseBuilt)
    {
        // The noise depends on nothing, so it is built once, whatever the mode; the shadow map below
        // and the sky and the probe read it.
        for (nvrhi::ITexture* image : {shape, detail, weather})
        {
            commandList->setTextureState(image, nvrhi::AllSubresources, States::UnorderedAccess);
        }
        commandList->commitBarriers();
        const uint32_t groups = GroupCount(kCloudNoiseSizes[kCloudShape], 4);
        Dispatch(commandList, kCloudNoisePipeline, frameBindingSet, set, groups, groups, groups);
        const uint32_t weatherGroups = GroupCount(kCloudNoiseSizes[kCloudWeather], 8);
        Dispatch(commandList, kCloudWeatherPipeline, frameBindingSet, set, weatherGroups, weatherGroups, 1);
        for (nvrhi::ITexture* image : {shape, detail, weather})
        {
            commandList->setTextureState(image, nvrhi::AllSubresources, States::ShaderResource);
        }
        m_cloudNoiseBuilt = true;
        m_cloudWeatherLife = cloudLife != nullptr ? *cloudLife : glm::vec4(-1.0f);
    }
    else if (cloudLife != nullptr && *cloudLife != m_cloudWeatherLife)
    {
        // The plumes have moved on in their lives (cloud_weather.comp reads the phases from the
        // frame's uniforms).
        commandList->setTextureState(weather, nvrhi::AllSubresources, States::UnorderedAccess);
        commandList->commitBarriers();
        const uint32_t weatherGroups = GroupCount(kCloudNoiseSizes[kCloudWeather], 8);
        Dispatch(commandList, kCloudWeatherPipeline, frameBindingSet, set, weatherGroups, weatherGroups, 1);
        commandList->setTextureState(weather, nvrhi::AllSubresources, States::ShaderResource);
        m_cloudWeatherLife = *cloudLife;
    }

    if (parameters != nullptr)
    {
        if (!m_staticLutParameters.has_value() || !(*m_staticLutParameters == *parameters))
        {
            commandList->setTextureState(transmittance, nvrhi::AllSubresources, States::UnorderedAccess);
            commandList->commitBarriers();
            Dispatch(commandList, kTransmittance, frameBindingSet, set, GroupCount(256, 8), GroupCount(64, 8), 1);
            commandList->setTextureState(transmittance, nvrhi::AllSubresources, States::ShaderResource);
            commandList->setTextureState(multiScattering, nvrhi::AllSubresources, States::UnorderedAccess);
            commandList->commitBarriers();
            Dispatch(commandList, kMultiScattering, frameBindingSet, set, 32, 32, 1);
            m_staticLutParameters = *parameters;
        }
        // The sky view samples the multiple scattering.
        commandList->setTextureState(multiScattering, nvrhi::AllSubresources, States::ShaderResource);
        commandList->setTextureState(skyView, nvrhi::AllSubresources, States::UnorderedAccess);
        commandList->setTextureState(shadow, nvrhi::AllSubresources, States::UnorderedAccess);
        commandList->commitBarriers();
        Dispatch(commandList, kSkyView, frameBindingSet, set, GroupCount(192, 8), GroupCount(108, 8), 1);
        // Every frame, as the map follows the camera; all ones when the clouds are off.
        Dispatch(commandList, kCloudShadowPipeline, frameBindingSet, set, GroupCount(kCloudShadowSize, 8), GroupCount(kCloudShadowSize, 8), 1);
        // The SH projection samples the sky-view LUT written above.
        commandList->setTextureState(skyView, nvrhi::AllSubresources, States::ShaderResource);
        commandList->setTextureState(shadow, nvrhi::AllSubresources, States::ShaderResource);
        commandList->setBufferState(m_irradianceHandle, States::UnorderedAccess);
        commandList->commitBarriers();
        Dispatch(commandList, kIrradiancePipeline, frameBindingSet, set, 1, 1, 1);

        // A copy for the CPU, read once this slot's fence has signaled; NVRHI has no state for the
        // host's read, so that barrier is native.
        commandList->setBufferState(m_irradianceHandle, States::CopySource);
        commandList->commitBarriers();
        commandList->copyBuffer(readback.handle, 0, m_irradianceHandle, 0, kIrradianceBytes);
        VkMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
    }
    readback.written = parameters != nullptr;
}

void VulkanAtmosphere::Dispatch(
    nvrhi::ICommandList* commandList,
    size_t pipeline,
    nvrhi::IBindingSet* frameBindingSet,
    nvrhi::IBindingSet* set,
    uint32_t x,
    uint32_t y,
    uint32_t z,
    const uint32_t* constants) const
{
    nvrhi::ComputeState state;
    state.pipeline = m_pipelines[pipeline];
    state.bindings = {frameBindingSet, set};
    commandList->setComputeState(state);
    // Every pipeline's layout has the push constants; NVRHI's validation drops a dispatch without them.
    static constexpr std::array<uint32_t, 4> kNoConstants{};
    commandList->setPushConstants(constants != nullptr ? constants : kNoConstants.data(), kPushConstantBytes);
    commandList->dispatch(x, y, z);
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

void VulkanAtmosphere::CreateBindingLayout()
{
    using Item = nvrhi::BindingLayoutItem;
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 1;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    desc.bindings = {
        Item::Texture_UAV(kTransmittance),
        Item::Texture_UAV(kMultiScattering),
        Item::Texture_UAV(kSkyView),
        Item::Texture_UAV(kAerialPerspectiveBinding),
        Item::Texture_SRV(kTransmittanceSampledBinding),
        Item::Sampler(kTransmittanceSampledBinding + kSamplerBindingOffset),
        Item::Texture_SRV(kMultiScatteringSampledBinding),
        Item::Sampler(kMultiScatteringSampledBinding + kSamplerBindingOffset),
        Item::RawBuffer_UAV(kIrradianceBinding),
        Item::Texture_UAV(7),
        Item::Texture_UAV(8),
        Item::Texture_UAV(9),
        Item::Texture_UAV(10),
        Item::Texture_UAV(kCloudTargetBinding),
        Item::Texture_UAV(kCloudResolvedBinding),
        Item::Texture_SRV(kCloudHistoryBinding),
        Item::Sampler(kCloudHistoryBinding + kSamplerBindingOffset),
        Item::RawBuffer_SRV(kPlumeBinding),
        Item::PushConstants(0, kPushConstantBytes)};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, desc, "Failed to create the atmosphere binding layout");
}

void VulkanAtmosphere::CreatePipelines(nvrhi::IBindingLayout* frameSetLayout)
{
    for (size_t pipeline = 0; pipeline < kPipelineCount; ++pipeline)
    {
        m_pipelines[pipeline] = CreateNvrhiComputePipeline(m_nvrhiDevice, kShaderNames[pipeline], {frameSetLayout, m_setLayout});
    }
}

void VulkanAtmosphere::DestroyHandles()
{
    m_pipelines = {};
    m_placeholderView.reset();
    m_setLayout = nullptr;
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
