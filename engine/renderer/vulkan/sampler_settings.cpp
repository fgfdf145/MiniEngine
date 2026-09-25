#include "sampler_settings.h"

namespace me
{

namespace
{
VkSamplerAddressMode ToAddressMode(TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::MirroredRepeat:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter ToFilter(TextureFilter filter)
{
    return filter == TextureFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}
}

VkSamplerCreateInfo BuildTextureSamplerInfo(const TextureSampler& sampler, float maxAnisotropy)
{
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = ToFilter(sampler.magFilter);
    info.minFilter = ToFilter(sampler.minFilter);
    info.addressModeU = ToAddressMode(sampler.wrapS);
    info.addressModeV = ToAddressMode(sampler.wrapT);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.mipmapMode = sampler.mipFilter == TextureMipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.minLod = 0.0f;
    info.maxLod = sampler.mipFilter == TextureMipFilter::None ? 0.25f : VK_LOD_CLAMP_NONE;
    info.mipLodBias = 0.0f;
    const bool anisotropic = maxAnisotropy > 0.0f && sampler.magFilter == TextureFilter::Linear &&
                             sampler.minFilter == TextureFilter::Linear && sampler.mipFilter != TextureMipFilter::None;
    info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = anisotropic ? maxAnisotropy : 1.0f;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_ALWAYS;
    return info;
}
}
