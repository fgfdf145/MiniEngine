#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <engine/scene/material_graph.h>

#include <vulkan/vulkan.h>

#include <map>
#include <tuple>

namespace me
{

// The Vulkan sampler a material texture slot's glTF sampler asks for. maxAnisotropy is the device's
// limit to use, 0 on a device without anisotropic filtering. Anisotropy is enabled only where both
// filters are linear and mipmaps are used: nearest filtering asks for texel-exact results. A
// sampler without mipmaps samples the base level alone (maxLod 0.25 with nearest mipmap mode, the
// Vulkan specification's recipe for GL's non-mipmapped minification). The default TextureSampler
// gives the sampler every texture had before glTF samplers were read.
VkSamplerCreateInfo BuildTextureSamplerInfo(const TextureSampler& sampler, float maxAnisotropy);

// One VkSampler per distinct TextureSampler, created on first use and destroyed with the cache.
// Material descriptors pair a texture's image view with the sampler its slot asks for, since one
// texture file is shared by every material that names it.
class VulkanSamplerCache
{
  public:
    VulkanSamplerCache(VkDevice device, float maxAnisotropy);
    ~VulkanSamplerCache();
    VulkanSamplerCache(const VulkanSamplerCache&) = delete;
    VulkanSamplerCache& operator=(const VulkanSamplerCache&) = delete;

    VkSampler Get(const TextureSampler& sampler);

  private:
    using Key = std::tuple<TextureWrap, TextureWrap, TextureFilter, TextureFilter, TextureMipFilter>;

    VkDevice m_device = VK_NULL_HANDLE;
    float m_maxAnisotropy = 0.0f;
    std::map<Key, VkSampler> m_samplers;
};
}
