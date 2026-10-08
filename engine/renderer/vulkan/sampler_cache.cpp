#include "sampler_settings.h"

#include "common.h"

namespace me
{

VulkanSamplerCache::VulkanSamplerCache(nvrhi::IDevice* device, float maxAnisotropy)
    : m_device(device), m_maxAnisotropy(maxAnisotropy)
{
}

nvrhi::ISampler* VulkanSamplerCache::Get(const TextureSampler& sampler)
{
    const Key key{sampler.wrapS, sampler.wrapT, sampler.magFilter, sampler.minFilter, sampler.mipFilter};
    if (const auto found = m_samplers.find(key); found != m_samplers.end())
    {
        return found->second;
    }
    nvrhi::SamplerHandle created = m_device->createSampler(BuildTextureSamplerDesc(sampler, m_maxAnisotropy));
    if (!created)
    {
        throw std::runtime_error("Failed to create a material texture sampler");
    }
    return m_samplers.emplace(key, std::move(created)).first->second;
}

VkSampler VulkanSamplerCache::GetNative(const TextureSampler& sampler)
{
    return Get(sampler)->getNativeObject(nvrhi::ObjectTypes::VK_Sampler);
}
}
