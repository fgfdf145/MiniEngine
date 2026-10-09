#include "sampler_settings.h"

#include <stdexcept>

namespace me
{

namespace
{
nvrhi::SamplerAddressMode ToAddressMode(TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return nvrhi::SamplerAddressMode::Clamp;
    case TextureWrap::MirroredRepeat:
        return nvrhi::SamplerAddressMode::Mirror;
    default:
        return nvrhi::SamplerAddressMode::Repeat;
    }
}
}

nvrhi::SamplerDesc BuildTextureSamplerDesc(const TextureSampler& sampler, float maxAnisotropy)
{
    nvrhi::SamplerDesc desc;
    desc.magFilter = sampler.magFilter == TextureFilter::Linear;
    desc.minFilter = sampler.minFilter == TextureFilter::Linear;
    desc.mipFilter = sampler.mipFilter == TextureMipFilter::Linear;
    desc.addressU = ToAddressMode(sampler.wrapS);
    desc.addressV = ToAddressMode(sampler.wrapT);
    desc.addressW = nvrhi::SamplerAddressMode::Repeat;
    desc.minLod = 0.0f;
    desc.maxLod = sampler.mipFilter == TextureMipFilter::None ? 0.25f : VK_LOD_CLAMP_NONE;
    desc.mipBias = 0.0f;
    const bool anisotropic = maxAnisotropy > 0.0f && sampler.magFilter == TextureFilter::Linear &&
                             sampler.minFilter == TextureFilter::Linear && sampler.mipFilter != TextureMipFilter::None;
    desc.maxAnisotropy = anisotropic ? maxAnisotropy : 1.0f;
    desc.borderColor = nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f);
    return desc;
}

nvrhi::SamplerDesc BuildClampSamplerDesc(bool linear)
{
    nvrhi::SamplerDesc desc;
    desc.magFilter = linear;
    desc.minFilter = linear;
    desc.mipFilter = false;
    desc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    desc.minLod = 0.0f;
    desc.maxLod = 0.0f;
    desc.maxAnisotropy = 1.0f;
    desc.borderColor = nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f);
    return desc;
}

// Mixed radix over the fields, mipFilter fastest, so the default TextureSampler (every field 0) is 0.
uint32_t VulkanSamplerCache::IndexOf(const TextureSampler& sampler)
{
    uint32_t index = static_cast<uint32_t>(sampler.wrapS);
    index = index * 3u + static_cast<uint32_t>(sampler.wrapT);
    index = index * 2u + static_cast<uint32_t>(sampler.magFilter);
    index = index * 2u + static_cast<uint32_t>(sampler.minFilter);
    index = index * 3u + static_cast<uint32_t>(sampler.mipFilter);
    return index;
}

TextureSampler VulkanSamplerCache::SamplerAt(uint32_t index)
{
    if (index >= kSamplerCount)
    {
        throw std::out_of_range("No TextureSampler has this index");
    }
    TextureSampler sampler;
    sampler.mipFilter = static_cast<TextureMipFilter>(index % 3u);
    index /= 3u;
    sampler.minFilter = static_cast<TextureFilter>(index % 2u);
    index /= 2u;
    sampler.magFilter = static_cast<TextureFilter>(index % 2u);
    index /= 2u;
    sampler.wrapT = static_cast<TextureWrap>(index % 3u);
    sampler.wrapS = static_cast<TextureWrap>(index / 3u);
    return sampler;
}
}
