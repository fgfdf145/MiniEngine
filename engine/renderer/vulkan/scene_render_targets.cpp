#include "scene_render_targets.h"

#include "command.h"
#include "format_support.h"

#include "../imgui/imgui_impl_vulkan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace me
{

namespace
{
bool HasStencilComponent(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

template <typename Handle>
ImTextureID ToImTextureId(Handle handle)
{
    if constexpr (std::is_pointer_v<Handle>)
    {
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(handle));
    }
    else
    {
        return static_cast<ImTextureID>(handle);
    }
}
}

SceneRenderTargets::SceneRenderTargets(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkFormat ldrFormat,
    VkExtent2D extent,
    uint32_t swapchainImageCount)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_extent({std::max(extent.width, 1u),
                std::max(extent.height, 1u)})
{
    SelectFormats(ldrFormat);
    CreateSampler();
    CreateImages(swapchainImageCount);
}

SceneRenderTargets::~SceneRenderTargets()
{
    ReleaseImages();

    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
    }
}

VkFormat SceneRenderTargets::GetFormat(RenderTargetId target) const
{
    return Describe(target).format;
}

VkImageAspectFlags SceneRenderTargets::GetAspect(RenderTargetId target) const
{
    return Describe(target).aspect;
}

VkImage SceneRenderTargets::GetImage(RenderTargetId target, uint32_t index) const
{
    return Describe(target).images.at(index).image;
}

VkImageView SceneRenderTargets::GetView(RenderTargetId target, uint32_t index) const
{
    return Describe(target).images.at(index).view;
}

VkImageView SceneRenderTargets::GetSampledView(RenderTargetId target, uint32_t index) const
{
    const TargetImage& image = Describe(target).images.at(index);
    return image.sampledView != VK_NULL_HANDLE ? image.sampledView : image.view;
}

VkExtent2D SceneRenderTargets::GetExtent() const
{
    return m_extent;
}

bool SceneRenderTargets::MatchesExtent(VkExtent2D extent) const
{
    return m_extent.width == std::max(extent.width, 1u) &&
           m_extent.height == std::max(extent.height, 1u);
}

uint32_t SceneRenderTargets::GetTransientCopyCount() const
{
    return static_cast<uint32_t>(VulkanCommandContext::kMaxFramesInFlight);
}

uint32_t SceneRenderTargets::GetLdrCopyCount() const
{
    return m_swapchainImageCount;
}

uint32_t SceneRenderTargets::ResolveIndex(RenderTargetId target, uint32_t imageIndex, uint32_t frameSlot) const
{
    return target == RenderTargetId::SceneLdr ? imageIndex : frameSlot;
}

ImTextureID SceneRenderTargets::GetLdrTextureId(uint32_t imageIndex) const
{
    return ToImTextureId(Describe(RenderTargetId::SceneLdr).images.at(imageIndex).imguiBinding);
}

void SceneRenderTargets::ReleaseImages()
{
    DestroyImages(m_targets);
    for (TargetDescription& description : m_targets)
    {
        description.images.clear();
    }
}

void SceneRenderTargets::Rebuild(VkExtent2D extent, uint32_t swapchainImageCount)
{
    const VkExtent2D clamped = {std::max(extent.width, 1u), std::max(extent.height, 1u)};

    // Snapshot what is live, build the replacement into the members, and put the snapshot back
    // if anything throws. A failure therefore leaves the previous, still-valid set in place.
    std::array<TargetDescription, kRenderTargetCount> previous = std::move(m_targets);
    const VkExtent2D previousExtent = m_extent;
    const uint32_t previousImageCount = m_swapchainImageCount;

    // Only the four scalar description fields are carried over; the images vectors start empty
    // because CreateImages fills them. Copying whole TargetDescriptions instead would duplicate
    // every live VkImage handle into a second owner, and the allocation that copy needs sits
    // outside the try below — a throw there would leave m_targets moved-from with nothing able to
    // restore it.
    for (size_t index = 0; index < m_targets.size(); ++index)
    {
        TargetDescription& description = m_targets[index];
        description.format = previous[index].format;
        description.usage = previous[index].usage;
        description.aspect = previous[index].aspect;
        description.bindToImGui = previous[index].bindToImGui;
        // A moved-from vector is valid but unspecified, and clear() neither allocates nor throws,
        // so this is what makes "starts empty" a guarantee rather than an observation.
        description.images.clear();
    }
    m_extent = clamped;

    try
    {
        CreateImages(swapchainImageCount);
    }
    catch (...)
    {
        DestroyImages(m_targets);
        m_targets = std::move(previous);
        m_extent = previousExtent;
        m_swapchainImageCount = previousImageCount;
        throw;
    }

    DestroyImages(previous);
}

VkFormatFeatureFlags SceneRenderTargets::QueryFormatFeatures(VkFormat format) const
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
    return properties.optimalTilingFeatures;
}

void SceneRenderTargets::SelectFormats(VkFormat ldrFormat)
{
    const FormatFeatureQuery query = [this](VkFormat format)
    {
        return QueryFormatFeatures(format);
    };

    // The depth target must additionally be sampleable: the deferred lighting pass reconstructs
    // world position from it. That requirement is why this cannot stay VulkanSceneViewport's
    // attachment-only search.
    static constexpr std::array<VkFormat, 3> kDepthCandidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT};
    static constexpr std::array<VkFormat, 1> kHdrCandidates = {
        VK_FORMAT_R16G16B16A16_SFLOAT};

    TargetDescription& depth = Describe(RenderTargetId::SceneDepth);
    depth.format = ChooseFormat(
        "depth",
        kDepthCandidates,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        query);
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depth.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencilComponent(depth.format))
    {
        depth.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    depth.bindToImGui = false;

    TargetDescription& hdr = Describe(RenderTargetId::SceneHdr);
    hdr.format = ChooseFormat(
        "HDR",
        kHdrCandidates,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        query);
    hdr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    hdr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Nothing displays the HDR target: the tone mapping pass samples it through its own
    // descriptor sets, and ImGui shows the LDR result instead.
    hdr.bindToImGui = false;

    TargetDescription& ldr = Describe(RenderTargetId::SceneLdr);
    ldr.format = ldrFormat;
    // Transfer source for --capture, which copies the viewport to a PNG.
    ldr.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ldr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    ldr.bindToImGui = true;

    // Written by the geometry pass; sampled by the lighting pass and the tone mapping debug views;
    // never bound to ImGui. Formats and channel contents follow the spec's G-buffer encoding table
    // as amended, and shaders/vulkan/gbuffer.frag is the writer. Only emissive has a fallback:
    // B10G11R11 is the one format in the table a desktop driver might plausibly lack as a color
    // attachment.
    //
    // GB1 is four channels, not the spec's two: .rg is the shading normal and .ba the geometric
    // normal, both octahedral. The lighting pass needs the geometric one for the shadow lookup's
    // normal offset, which triangle.frag takes along the interpolated vertex normal.
    static constexpr std::array<VkFormat, 1> kAlbedoCandidates = {VK_FORMAT_R8G8B8A8_SRGB};
    static constexpr std::array<VkFormat, 1> kNormalCandidates = {VK_FORMAT_R16G16B16A16_SFLOAT};
    static constexpr std::array<VkFormat, 1> kSurfaceCandidates = {VK_FORMAT_R8G8B8A8_UNORM};
    static constexpr std::array<VkFormat, 2> kEmissiveCandidates = {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};

    const auto describeGBufferTarget = [&](RenderTargetId target, std::string_view label, std::span<const VkFormat> candidates)
    {
        TargetDescription& description = Describe(target);
        description.format = ChooseFormat(
            label,
            candidates,
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            query);
        description.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        description.bindToImGui = false;
    };

    describeGBufferTarget(RenderTargetId::GBufferAlbedo, "G-buffer albedo", kAlbedoCandidates);
    describeGBufferTarget(RenderTargetId::GBufferNormal, "G-buffer normal", kNormalCandidates);
    describeGBufferTarget(RenderTargetId::GBufferSurface, "G-buffer surface", kSurfaceCandidates);
    describeGBufferTarget(RenderTargetId::GBufferEmissive, "G-buffer emissive", kEmissiveCandidates);

    // Motion vectors: current UV minus previous UV, written by the geometry pass. Both of the
    // features ChooseFormat asks for are mandatory for this format, so it has no fallback.
    // .ba carries the coat's normal (octahedral), which needs the half floats' precision.
    static constexpr std::array<VkFormat, 1> kVelocityCandidates = {VK_FORMAT_R16G16B16A16_SFLOAT};
    describeGBufferTarget(RenderTargetId::GBufferVelocity, "G-buffer velocity", kVelocityCandidates);

    // The material layers (GB5 specular, GB6 coat and anisotropy, GB7 sheen). RGBA8 is a mandatory
    // color attachment and sampled format.
    static constexpr std::array<VkFormat, 1> kLayerCandidates = {VK_FORMAT_R8G8B8A8_UNORM};
    describeGBufferTarget(RenderTargetId::GBufferSpecular, "G-buffer specular", kLayerCandidates);
    describeGBufferTarget(RenderTargetId::GBufferCoat, "G-buffer coat", kLayerCandidates);
    describeGBufferTarget(RenderTargetId::GBufferSheen, "G-buffer sheen", kLayerCandidates);

    // Visibility bitmask AO, written by compute through image stores. R32F is in the core list of
    // storage formats, so no shaderStorageImageExtendedFormats is needed, and it has no fallback.
    static constexpr std::array<VkFormat, 1> kAoCandidates = {VK_FORMAT_R32_SFLOAT};
    const auto describeAoTarget = [&](RenderTargetId target, std::string_view label)
    {
        TargetDescription& description = Describe(target);
        description.format = ChooseFormat(
            label,
            kAoCandidates,
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            query);
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        description.bindToImGui = false;
    };
    describeAoTarget(RenderTargetId::AoRaw, "AO trace");
    describeAoTarget(RenderTargetId::SceneAo, "AO");

    // Screen-space reflections, written by compute: rgb radiance, a confidence. RGBA16F is in the
    // core list of storage formats.
    static constexpr std::array<VkFormat, 1> kSsrCandidates = {VK_FORMAT_R16G16B16A16_SFLOAT};
    const auto describeSsrTarget = [&](RenderTargetId target, std::string_view label)
    {
        TargetDescription& description = Describe(target);
        description.format = ChooseFormat(
            label,
            kSsrCandidates,
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
            query);
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        description.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        description.bindToImGui = false;
    };
    describeSsrTarget(RenderTargetId::SsrRaw, "SSR trace");
    describeSsrTarget(RenderTargetId::SceneReflections, "Reflections");

    // TAA's output, written by compute. RGBA16F is in the core list of storage formats too.
    static constexpr std::array<VkFormat, 1> kTaaCandidates = {VK_FORMAT_R16G16B16A16_SFLOAT};
    TargetDescription& taa = Describe(RenderTargetId::SceneTaa);
    taa.format = ChooseFormat(
        "TAA",
        kTaaCandidates,
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        query);
    taa.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    taa.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    taa.bindToImGui = false;

    // CreateImages makes an image for every id in the enum. A target appended without a
    // description here would reach vkCreateImage with VK_FORMAT_UNDEFINED and fail far from the
    // cause; phase three appends GB4, so name the omission at the point it happens.
    for (size_t index = 0; index < m_targets.size(); ++index)
    {
        if (m_targets[index].format == VK_FORMAT_UNDEFINED)
        {
            throw std::runtime_error(
                "SelectFormats left render target " + std::to_string(index) + " without a format");
        }
    }
}

void SceneRenderTargets::CreateSampler()
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.mipLodBias = 0.0f;

    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create viewport sampler");
}

void SceneRenderTargets::CreateImages(uint32_t swapchainImageCount)
{
    m_swapchainImageCount = swapchainImageCount;

    for (size_t index = 0; index < m_targets.size(); ++index)
    {
        const RenderTargetId target = static_cast<RenderTargetId>(index);
        TargetDescription& description = m_targets[index];
        const uint32_t copyCount =
            target == RenderTargetId::SceneLdr ? swapchainImageCount : GetTransientCopyCount();

        description.images.assign(copyCount, TargetImage{});
        for (TargetImage& image : description.images)
        {
            CreateImage(description.format, description.usage, image);
            image.view = CreateImageView(image.image, description.format, description.aspect);
            if ((description.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
            {
                image.sampledView = CreateImageView(image.image, description.format, VK_IMAGE_ASPECT_DEPTH_BIT);
            }
            if (description.bindToImGui)
            {
                image.imguiBinding = ImGui_ImplVulkan_AddTexture(
                    m_sampler,
                    image.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }
}

void SceneRenderTargets::DestroyImages(std::array<TargetDescription, kRenderTargetCount>& targets) const
{
    for (TargetDescription& description : targets)
    {
        for (TargetImage& image : description.images)
        {
            if (image.imguiBinding != VK_NULL_HANDLE)
            {
                ImGui_ImplVulkan_RemoveTexture(image.imguiBinding);
            }
            if (image.sampledView != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, image.sampledView, nullptr);
            }
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
        }
    }
}

uint32_t SceneRenderTargets::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
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

    throw std::runtime_error("Failed to find suitable viewport image memory type");
}

void SceneRenderTargets::CreateImage(VkFormat format, VkImageUsageFlags usage, TargetImage& target) const
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_extent.width;
    imageInfo.extent.height = m_extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &target.image), "Failed to create viewport image");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(m_device, target.image, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &target.memory), "Failed to allocate viewport image memory");
    CheckVulkan(vkBindImageMemory(m_device, target.image, target.memory, 0), "Failed to bind viewport image memory");
}

VkImageView SceneRenderTargets::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &imageView), "Failed to create viewport image view");
    return imageView;
}

SceneRenderTargets::TargetDescription& SceneRenderTargets::Describe(RenderTargetId target)
{
    return m_targets.at(static_cast<size_t>(target));
}

const SceneRenderTargets::TargetDescription& SceneRenderTargets::Describe(RenderTargetId target) const
{
    return m_targets.at(static_cast<size_t>(target));
}
}
