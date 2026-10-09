#include "environment_probe.h"

#include "compute_pass_util.h"
#include "nvrhi_pass.h"
#include "nvrhi_resources.h"
#include "sampler_settings.h"

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

// The prefilter's sampler of the radiance cube: binding 1 + 64.
constexpr uint32_t kRadianceSamplerBinding = 1 + 64;

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
    nvrhi::IBindingLayout* frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_downsample(nvrhiDevice)
{
    try
    {
        m_radiance = CreateCube(
            kRadianceMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_prefiltered = CreateCube(
            kPrefilterMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        nvrhi::SamplerDesc samplerDesc = BuildClampSamplerDesc(true);
        samplerDesc.mipFilter = true;
        samplerDesc.maxLod = static_cast<float>(kRadianceMipCount);
        m_sampler = CreateNvrhiSampler(nvrhiDevice, samplerDesc, "Failed to create the environment probe sampler");

        CreateBindings(frameSetLayout);
        m_radianceMipSets = m_downsample.CreateBindingSets(m_radiance.texture, 1);
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
    nvrhi::ICommandList* commandList,
    nvrhi::IBindingSet* frameBindingSet,
    bool physicalSky,
    const EnvironmentUniformData& environment)
{
    const bool initializing = !m_imagesInitialized;
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

    // Between Records the radiance cube rests in GENERAL and the prefiltered one in
    // SHADER_READ_ONLY_OPTIMAL, where set 0 samples it; new ones come out of UNDEFINED. The scope's
    // closing barriers make this Record's writes visible to every later reader.
    using States = nvrhi::ResourceStates;
    nvrhi::ITexture* radiance = m_radiance.texture;
    nvrhi::ITexture* prefiltered = m_prefiltered.texture;
    const NvrhiPassScope scope(
        commandList,
        {{radiance, initializing ? States::Common : States::UnorderedAccess, States::UnorderedAccess},
         {prefiltered, initializing ? States::Common : States::ShaderResource, States::ShaderResource}});
    if (initializing)
    {
        // Set 0 binding 8 names the prefiltered cube for every draw: both start black.
        ClearTextureFloat(commandList, radiance, nvrhi::Color(0.0f));
        ClearTextureFloat(commandList, prefiltered, nvrhi::Color(0.0f));
        m_imagesInitialized = true;
    }
    if (!capture)
    {
        return;
    }

    commandList->setTextureState(radiance, nvrhi::AllSubresources, States::UnorderedAccess);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_capturePipeline;
    state.bindings = {frameBindingSet, m_bindingSets[0]};
    commandList->setComputeState(state);
    // The capture reads no push constants, but its layout has the prefilter's, and NVRHI's
    // validation drops a dispatch without them.
    const PrefilterConstants noConstants{};
    commandList->setPushConstants(&noConstants, sizeof(noConstants));
    commandList->dispatch(GroupCount(kCubeSize), GroupCount(kCubeSize), 6);

    // The radiance cube's mips, each from the one above, which the prefilter samples.
    m_downsample.Record(commandList, radiance, 1, m_radianceMipSets);

    commandList->setTextureState(radiance, nvrhi::AllSubresources, States::ShaderResource);
    commandList->setTextureState(prefiltered, nvrhi::AllSubresources, States::UnorderedAccess);
    commandList->commitBarriers();
    state.pipeline = m_prefilterPipeline;
    for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
    {
        state.bindings = {frameBindingSet, m_bindingSets[mip]};
        commandList->setComputeState(state);
        PrefilterConstants constants{};
        constants.roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMipCount - 1);
        constants.size = kCubeSize >> mip;
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(GroupCount(constants.size), GroupCount(constants.size), 6);
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
    CheckVulkan(CreateNativeImageView(m_device, &viewInfo, nullptr, &cube.cubeView), "Failed to create an environment cube view");
    return cube;
}

void VulkanEnvironmentProbe::CreateBindings(nvrhi::IBindingLayout* frameSetLayout)
{
    // 0 the radiance cube's mip 0 (capture output, the six faces as an array), 1 the radiance cube
    // sampled (its sampler at 1 + 64), 2 one prefiltered mip (prefilter output); the capture uses
    // only the first, the prefilter the other two and the push constants.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 1;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = ShaderBindingOffsets();
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_UAV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Sampler(kRadianceSamplerBinding),
        nvrhi::BindingLayoutItem::Texture_UAV(2),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PrefilterConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, layoutDesc, "Failed to create the environment probe binding layout");

    for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_UAV(
                0, m_radiance.texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray),
            nvrhi::BindingSetItem::Texture_SRV(
                1, m_radiance.texture, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube),
            nvrhi::BindingSetItem::Sampler(kRadianceSamplerBinding, m_sampler),
            nvrhi::BindingSetItem::Texture_UAV(
                2, m_prefiltered.texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(PrefilterConstants))};
        m_bindingSets[mip] = CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create an environment probe binding set");
    }

    m_capturePipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "environment_capture.comp.spv", {frameSetLayout, m_setLayout});
    m_prefilterPipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "environment_prefilter.comp.spv", {frameSetLayout, m_setLayout});
}

void VulkanEnvironmentProbe::DestroyHandles()
{
    m_capturePipeline = nullptr;
    m_prefilterPipeline = nullptr;
    m_bindingSets = {};
    m_setLayout = nullptr;
    m_sampler = nullptr;
    for (CubeImage* cube : {&m_radiance, &m_prefiltered})
    {
        if (cube->cubeView != VK_NULL_HANDLE)
        {
            DestroyNativeImageView(m_device, cube->cubeView, nullptr);
        }
        // The image and its memory go with the texture.
        *cube = CubeImage{};
    }
}
}
