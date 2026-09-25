#include "sampler_settings.h"

#include "common.h"

namespace me
{

VulkanSamplerCache::VulkanSamplerCache(VkDevice device, float maxAnisotropy)
    : m_device(device), m_maxAnisotropy(maxAnisotropy)
{
}

VulkanSamplerCache::~VulkanSamplerCache()
{
    for (const auto& [key, sampler] : m_samplers)
    {
        vkDestroySampler(m_device, sampler, nullptr);
    }
}

VkSampler VulkanSamplerCache::Get(const TextureSampler& sampler)
{
    const Key key{sampler.wrapS, sampler.wrapT, sampler.magFilter, sampler.minFilter, sampler.mipFilter};
    if (const auto found = m_samplers.find(key); found != m_samplers.end())
    {
        return found->second;
    }
    const VkSamplerCreateInfo info = BuildTextureSamplerInfo(sampler, m_maxAnisotropy);
    VkSampler created = VK_NULL_HANDLE;
    CheckVulkan(vkCreateSampler(m_device, &info, nullptr, &created), "Failed to create a material texture sampler");
    m_samplers.emplace(key, created);
    return created;
}
}
