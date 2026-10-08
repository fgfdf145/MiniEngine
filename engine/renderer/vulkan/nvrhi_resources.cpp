#include "nvrhi_resources.h"

#include <string>

namespace me
{

nvrhi::TextureHandle CreateNvrhiImage(nvrhi::IDevice* device, const VkImageCreateInfo& info, VkImage& image, const char* failureMessage)
{
    constexpr VkImageUsageFlags kKnownUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    constexpr VkImageCreateFlags kKnownFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (info.tiling != VK_IMAGE_TILING_OPTIMAL || info.sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        info.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED || (info.usage & ~kKnownUsage) != 0 || (info.flags & ~kKnownFlags) != 0 ||
        info.pNext != nullptr)
    {
        throw std::runtime_error(std::string(failureMessage) + ": an image NVRHI cannot describe");
    }

    nvrhi::TextureDesc desc;
    desc.width = info.extent.width;
    desc.height = info.extent.height;
    desc.depth = info.extent.depth;
    desc.arraySize = info.arrayLayers;
    desc.mipLevels = info.mipLevels;
    desc.sampleCount = static_cast<uint32_t>(info.samples);
    desc.format = ToNvrhiFormat(info.format);
    if (desc.format == nvrhi::Format::UNKNOWN)
    {
        throw std::runtime_error(std::string(failureMessage) + ": a format NVRHI has no name for");
    }
    const bool cube = (info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
    switch (info.imageType)
    {
    case VK_IMAGE_TYPE_1D:
        desc.dimension = info.arrayLayers > 1 ? nvrhi::TextureDimension::Texture1DArray : nvrhi::TextureDimension::Texture1D;
        break;
    case VK_IMAGE_TYPE_3D:
        desc.dimension = nvrhi::TextureDimension::Texture3D;
        break;
    default:
        if (cube)
        {
            desc.dimension = info.arrayLayers > 6 ? nvrhi::TextureDimension::TextureCubeArray : nvrhi::TextureDimension::TextureCube;
        }
        else if (info.samples != VK_SAMPLE_COUNT_1_BIT)
        {
            desc.dimension = info.arrayLayers > 1 ? nvrhi::TextureDimension::Texture2DMSArray : nvrhi::TextureDimension::Texture2DMS;
        }
        else
        {
            desc.dimension = info.arrayLayers > 1 ? nvrhi::TextureDimension::Texture2DArray : nvrhi::TextureDimension::Texture2D;
        }
        break;
    }
    if (cube && info.imageType != VK_IMAGE_TYPE_2D)
    {
        throw std::runtime_error(std::string(failureMessage) + ": an image NVRHI cannot describe");
    }
    desc.isShaderResource = (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    desc.isUAV = (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    desc.isRenderTarget = (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    // MUTABLE_FORMAT, and EXTENDED_USAGE with it: a view in a format the image's usage does not allow.
    desc.isTypeless = (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0;
    desc.debugName = failureMessage;

    nvrhi::TextureHandle texture = device->createTexture(desc);
    if (!texture)
    {
        // NVRHI logs the VkResult. Running out of memory is what it is in practice, and the failure
        // callers of these images recover from.
        throw VulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, failureMessage);
    }
    image = ToNative<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image));
    return texture;
}

nvrhi::BufferHandle CreateNvrhiBuffer(
    nvrhi::IDevice* device,
    const VkBufferCreateInfo& info,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    const char* failureMessage,
    void** mapped)
{
    constexpr VkBufferUsageFlags kKnownUsage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    if (info.flags != 0 || info.sharingMode != VK_SHARING_MODE_EXCLUSIVE || (info.usage & ~kKnownUsage) != 0 ||
        info.pNext != nullptr || info.size == 0)
    {
        throw std::runtime_error(std::string(failureMessage) + ": a buffer NVRHI cannot describe");
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = info.size;
    desc.isVertexBuffer = (info.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0;
    desc.isIndexBuffer = (info.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0;
    desc.isConstantBuffer = (info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0;
    desc.isDrawIndirectArgs = (info.usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0;
    desc.canHaveUAVs = (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
    desc.canHaveRawViews = desc.canHaveUAVs;
    desc.isAccelStructBuildInput = (info.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) != 0;
    desc.isAccelStructStorage = (info.usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR) != 0;
    desc.isShaderBindingTable = (info.usage & VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR) != 0;
    if (properties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    {
        desc.cpuAccess = nvrhi::CpuAccessMode::None;
    }
    else if (properties == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    }
    else if ((properties & ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
    {
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    }
    else
    {
        throw std::runtime_error(std::string(failureMessage) + ": a memory kind NVRHI cannot allocate");
    }
    desc.debugName = failureMessage;

    nvrhi::BufferHandle handle = device->createBuffer(desc);
    if (!handle)
    {
        throw VulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, failureMessage);
    }
    buffer = ToNative<VkBuffer>(handle->getNativeObject(nvrhi::ObjectTypes::VK_Buffer));
    if (mapped != nullptr)
    {
        if (desc.cpuAccess == nvrhi::CpuAccessMode::None)
        {
            throw std::runtime_error(std::string(failureMessage) + ": device-local memory cannot be mapped");
        }
        *mapped = device->mapBuffer(handle, desc.cpuAccess);
        if (*mapped == nullptr)
        {
            throw VulkanError(VK_ERROR_MEMORY_MAP_FAILED, failureMessage);
        }
    }
    return handle;
}
}
