#include "sampler_settings.h"

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
}
