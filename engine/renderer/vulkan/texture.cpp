#include "texture.h"

#include "nvrhi_native.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
// The texel formats whose mips are built here, and how a texel turns into linear floats and back.
enum class TexelFormat
{
    Rgba8Srgb,
    Rgba8Unorm,
    Rgba16Float
};

float SrgbToLinear(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float linear)
{
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

uint8_t ToUnorm8(float value)
{
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

size_t TexelBytes(TexelFormat format)
{
    return format == TexelFormat::Rgba16Float ? 8 : 4;
}

// The image as linear floats, four a texel.
std::vector<float> Decode(const void* texels, uint32_t width, uint32_t height, TexelFormat format)
{
    const size_t count = static_cast<size_t>(width) * height * 4;
    std::vector<float> decoded(count);
    if (format == TexelFormat::Rgba16Float)
    {
        const auto* halves = static_cast<const uint16_t*>(texels);
        for (size_t index = 0; index < count; ++index)
        {
            decoded[index] = glm::unpackHalf1x16(halves[index]);
        }
        return decoded;
    }
    static const std::array<float, 256> kSrgb = []()
    {
        std::array<float, 256> table{};
        for (size_t value = 0; value < table.size(); ++value)
        {
            table[value] = SrgbToLinear(static_cast<float>(value) / 255.0f);
        }
        return table;
    }();
    const auto* bytes = static_cast<const uint8_t*>(texels);
    for (size_t index = 0; index < count; ++index)
    {
        // Alpha is linear in every format.
        const bool colour = format == TexelFormat::Rgba8Srgb && index % 4 != 3;
        decoded[index] = colour ? kSrgb[bytes[index]] : static_cast<float>(bytes[index]) / 255.0f;
    }
    return decoded;
}

std::vector<uint8_t> Encode(const std::vector<float>& linear, TexelFormat format)
{
    std::vector<uint8_t> encoded(linear.size() / 4 * TexelBytes(format));
    if (format == TexelFormat::Rgba16Float)
    {
        auto* halves = reinterpret_cast<uint16_t*>(encoded.data());
        for (size_t index = 0; index < linear.size(); ++index)
        {
            halves[index] = glm::packHalf1x16(linear[index]);
        }
        return encoded;
    }
    for (size_t index = 0; index < linear.size(); ++index)
    {
        const bool colour = format == TexelFormat::Rgba8Srgb && index % 4 != 3;
        encoded[index] = ToUnorm8(colour ? LinearToSrgb(linear[index]) : linear[index]);
    }
    return encoded;
}

// The next level down, as a linear blit makes it (vkCmdBlitImage, VK_FILTER_LINEAR): each target
// texel is the source bilinearly sampled at the target texel's centre, clamped to the edge; the 2x2
// average where the source size is even. sRGB images filter in linear space, as the blit did.
std::vector<float> Downsample(const std::vector<float>& source, uint32_t width, uint32_t height, uint32_t targetWidth, uint32_t targetHeight)
{
    std::vector<float> target(static_cast<size_t>(targetWidth) * targetHeight * 4);
    const float scaleX = static_cast<float>(width) / static_cast<float>(targetWidth);
    const float scaleY = static_cast<float>(height) / static_cast<float>(targetHeight);
    for (uint32_t y = 0; y < targetHeight; ++y)
    {
        const float sy = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;
        const int32_t y0 = static_cast<int32_t>(std::floor(sy));
        const float fy = sy - static_cast<float>(y0);
        const uint32_t rowA = static_cast<uint32_t>(std::clamp(y0, 0, static_cast<int32_t>(height) - 1));
        const uint32_t rowB = static_cast<uint32_t>(std::clamp(y0 + 1, 0, static_cast<int32_t>(height) - 1));
        for (uint32_t x = 0; x < targetWidth; ++x)
        {
            const float sx = (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
            const int32_t x0 = static_cast<int32_t>(std::floor(sx));
            const float fx = sx - static_cast<float>(x0);
            const uint32_t columnA = static_cast<uint32_t>(std::clamp(x0, 0, static_cast<int32_t>(width) - 1));
            const uint32_t columnB = static_cast<uint32_t>(std::clamp(x0 + 1, 0, static_cast<int32_t>(width) - 1));
            const float* a = &source[(static_cast<size_t>(rowA) * width + columnA) * 4];
            const float* b = &source[(static_cast<size_t>(rowA) * width + columnB) * 4];
            const float* c = &source[(static_cast<size_t>(rowB) * width + columnA) * 4];
            const float* d = &source[(static_cast<size_t>(rowB) * width + columnB) * 4];
            float* out = &target[(static_cast<size_t>(y) * targetWidth + x) * 4];
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                const float top = a[channel] + (b[channel] - a[channel]) * fx;
                const float bottom = c[channel] + (d[channel] - c[channel]) * fx;
                out[channel] = top + (bottom - top) * fy;
            }
        }
    }
    return target;
}
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const std::string& path,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_textureFormat(textureFormat)
{
    try
    {
        UploadTexture(TextureLoader::LoadRGBA8(path), uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const TextureData& textureData,
    VulkanUploadBatch& uploadBatch,
    VulkanTextureFormat textureFormat)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_textureFormat(textureFormat)
{
    // A throw out of a constructor skips the destructor, so whatever was created before the
    // failure is released here with the same call the destructor makes.
    try
    {
        UploadTexture(textureData, uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const HalfFloatTextureData& textureData,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!textureData.IsValid())
        {
            throw std::runtime_error("Cannot create a texture from invalid half-float data");
        }
        UploadTexels(
            textureData.texels.data(),
            static_cast<uint32_t>(textureData.width),
            static_cast<uint32_t>(textureData.height),
            VK_FORMAT_R16G16B16A16_SFLOAT,
            uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const FloatTextureData& equirectangular,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice),
      m_textureFormat(VulkanTextureFormat::LinearData)
{
    try
    {
        if (!equirectangular.IsValid())
        {
            throw std::runtime_error("Cannot create an environment map from invalid float data");
        }
        const uint32_t width = static_cast<uint32_t>(equirectangular.width);
        const uint32_t height = static_cast<uint32_t>(equirectangular.height);
        // Linear filtering of 32-bit floats is optional; NVRHI says sampling for filtered sampling.
        const nvrhi::FormatSupport support = m_nvrhiDevice->queryFormatSupport(nvrhi::Format::RGBA32_FLOAT);
        if ((support & nvrhi::FormatSupport::ShaderSample) == nvrhi::FormatSupport::ShaderSample)
        {
            UploadTexels(equirectangular.pixels.data(), width, height, VK_FORMAT_R32G32B32A32_SFLOAT, uploadBatch, false);
        }
        else
        {
            const HalfFloatTextureData packed = PackRgba16Float(equirectangular);
            UploadTexels(packed.texels.data(), width, height, VK_FORMAT_R16G16B16A16_SFLOAT, uploadBatch, false);
        }
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanTexture::VulkanTexture(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    nvrhi::IDevice* nvrhiDevice,
    const CompressedTexture& texture,
    VulkanUploadBatch& uploadBatch)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    try
    {
        UploadCompressedTexture(texture, uploadBatch);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

void VulkanTexture::UploadTexture(const TextureData& textureData, VulkanUploadBatch& uploadBatch)
{
    if (!textureData.IsValid())
    {
        throw std::runtime_error("Cannot create a texture from invalid pixel data");
    }
    UploadTexels(
        textureData.pixels.data(),
        static_cast<uint32_t>(textureData.width),
        static_cast<uint32_t>(textureData.height),
        GetVkFormat(),
        uploadBatch);
}

void VulkanTexture::UploadTexels(
    const void* texels,
    uint32_t width,
    uint32_t height,
    VkFormat vkFormat,
    VulkanUploadBatch& uploadBatch,
    bool generateMips)
{
    const bool mipmapped = generateMips && vkFormat != VK_FORMAT_R32G32B32A32_SFLOAT;
    m_mipLevels = mipmapped ? static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(std::max(width, height))))) + 1 : 1;
    CreateImage(width, height, m_mipLevels, vkFormat);

    const size_t texelBytes = vkFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    uploadBatch.WriteTexture(m_texture, 0, texels, width * texelBytes, static_cast<uint64_t>(width) * height * texelBytes);
    if (m_mipLevels > 1)
    {
        // The chain the blits made, built here: neither NVRHI nor D3D12 blits.
        const TexelFormat format = vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? TexelFormat::Rgba16Float
                                   : vkFormat == VK_FORMAT_R8G8B8A8_SRGB    ? TexelFormat::Rgba8Srgb
                                                                            : TexelFormat::Rgba8Unorm;
        std::vector<float> level = Decode(texels, width, height, format);
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;
        for (uint32_t mip = 1; mip < m_mipLevels; ++mip)
        {
            const uint32_t nextWidth = std::max(levelWidth / 2, 1u);
            const uint32_t nextHeight = std::max(levelHeight / 2, 1u);
            level = Downsample(level, levelWidth, levelHeight, nextWidth, nextHeight);
            const std::vector<uint8_t> encoded = Encode(level, format);
            uploadBatch.WriteTexture(m_texture, mip, encoded.data(), nextWidth * TexelBytes(format), encoded.size());
            levelWidth = nextWidth;
            levelHeight = nextHeight;
        }
    }
    CreateView(vkFormat);
}

void VulkanTexture::UploadCompressedTexture(const CompressedTexture& texture, VulkanUploadBatch& uploadBatch)
{
    if (texture.levels.empty())
    {
        throw std::runtime_error("Cannot create a texture from a compressed texture with no levels");
    }
    const VkFormat vkFormat = ToVkFormat(texture.format);
    m_mipLevels = static_cast<uint32_t>(texture.levels.size());
    CreateImage(texture.levels[0].width, texture.levels[0].height, m_mipLevels, vkFormat);

    // Every format here is 16 bytes a 4x4 block; a level's rows are rows of blocks.
    for (uint32_t levelIndex = 0; levelIndex < m_mipLevels; ++levelIndex)
    {
        const CompressedTextureLevel& level = texture.levels[levelIndex];
        const uint64_t rowPitch = static_cast<uint64_t>((level.width + 3) / 4) * 16;
        uploadBatch.WriteTexture(m_texture, levelIndex, level.blocks.data(), rowPitch, level.blocks.size());
    }
    CreateView(vkFormat);
}

VkFormat VulkanTexture::ToVkFormat(CompressedTextureFormat format)
{
    switch (format)
    {
    case CompressedTextureFormat::Bc7Srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case CompressedTextureFormat::Bc7Unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case CompressedTextureFormat::Bc5Unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    }
    throw std::runtime_error("Unknown compressed texture format");
}

void VulkanTexture::CreateView(VkFormat vkFormat)
{
    // The native view the Vulkan descriptor writes take (the ray texture table's); NVRHI makes its
    // own views, and D3D12 needs none of this.
    if (m_device == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE)
    {
        return;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageView), "Failed to create texture image view");
}

VkFormat VulkanTexture::GetVkFormat() const
{
    return m_textureFormat == VulkanTextureFormat::SrgbColor ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

VulkanTexture::~VulkanTexture()
{
    DestroyHandles();
}

void VulkanTexture::DestroyHandles()
{
    if (m_imageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    // The image goes before the range it is bound to.
    m_texture = nullptr;
    m_image = VK_NULL_HANDLE;
    VulkanMemoryPool::Free(m_memory);
}

VkImageView VulkanTexture::GetImageView() const
{
    return m_imageView;
}

nvrhi::ITexture* VulkanTexture::GetNvrhiTexture() const
{
    return m_texture;
}

void VulkanTexture::CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format)
{
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = mipLevels;
    desc.format = ToNvrhiFormat(format);
    if (desc.format == nvrhi::Format::UNKNOWN)
    {
        throw std::runtime_error("A texture format NVRHI has no name for");
    }
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.isShaderResource = true;
    desc.isVirtual = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Texture";
    m_texture = m_nvrhiDevice->createTexture(desc);
    if (!m_texture)
    {
        throw std::runtime_error("Failed to create texture image");
    }
    m_image = ToNative<VkImage>(m_texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));

    // The image is the caller's to release (DestroyHandles), so only the memory is undone here.
    m_memory = VulkanMemoryPool::AllocateFor(m_nvrhiDevice, m_texture);
    if (!m_nvrhiDevice->bindTextureMemory(m_texture, m_memory.heap, m_memory.offset))
    {
        VulkanMemoryPool::Free(m_memory);
        throw std::runtime_error("Failed to bind texture image memory");
    }
}
}
