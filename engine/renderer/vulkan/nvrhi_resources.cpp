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
}
