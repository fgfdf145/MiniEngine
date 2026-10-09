#include "material_set_cache.h"

#include "nvrhi_native.h"
#include "nvrhi_pass.h"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace me
{

size_t VulkanMaterialSetCache::KeyHash::operator()(const Key& key) const
{
    size_t hash = 1469598103934665603ull;
    for (const TextureDescriptorBinding& binding : key)
    {
        hash ^= std::hash<const void*>{}(binding.imageView) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<const void*>{}(binding.sampler) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    return hash;
}

bool VulkanMaterialSetCache::KeyEqual::operator()(const Key& a, const Key& b) const
{
    for (size_t index = 0; index < a.size(); ++index)
    {
        if (a[index].imageView != b[index].imageView || a[index].sampler != b[index].sampler)
        {
            return false;
        }
    }
    return true;
}

VulkanMaterialSetCache::VulkanMaterialSetCache(nvrhi::IDevice* device, nvrhi::IBindingLayout* materialSetLayout)
    : m_device(device),
      m_layout(materialSetLayout)
{
}

VulkanMaterialSetCache::~VulkanMaterialSetCache() = default;

VulkanMaterialSetCache::Key VulkanMaterialSetCache::KeyOf(const MaterialTextureBinding& binding)
{
    // In binding order (kMaterialTextureBindingCount): the order the material set layout declares.
    return Key{
        binding.baseColor,
        binding.normal,
        binding.metallic,
        binding.roughness,
        binding.occlusion,
        binding.emissive,
        binding.secondaryBaseColor,
        binding.secondaryNormal,
        binding.secondaryMetallic,
        binding.secondaryRoughness,
        binding.secondaryOcclusion,
        binding.secondaryEmissive,
        binding.blendMask,
        binding.clearcoat,
        binding.clearcoatRoughness,
        binding.sheenColor,
        binding.sheenRoughness,
        binding.anisotropy,
        binding.specular,
        binding.specularColor,
        binding.clearcoatNormal,
        binding.iridescence,
        binding.iridescenceThickness,
        binding.transmission,
        binding.thickness,
        binding.diffuseTransmission,
        binding.diffuseTransmissionColor,
        binding.detailMask,
        binding.detailLayers[0],
        binding.detailLayers[1],
        binding.detailLayers[2],
        binding.detailLayers[3]};
}

VkDescriptorSet VulkanMaterialSetCache::Acquire(const MaterialTextureBinding& binding)
{
    const Key key = KeyOf(binding);
    if (const auto found = m_entries.find(key); found != m_entries.end())
    {
        return found->second.set;
    }

    // Each texture (binding b, NVRHI's view of the whole texture, as GetImageView is) and its sampler
    // (b + kMaterialSamplerBindingOffset).
    nvrhi::BindingSetDesc desc;
    desc.bindings.reserve(2 * kMaterialTextureBindingCount);
    for (uint32_t index = 0; index < kMaterialTextureBindingCount; ++index)
    {
        if (key[index].texture == nullptr || key[index].nvrhiSampler == nullptr)
        {
            throw std::runtime_error("A material texture binding has no NVRHI texture or sampler");
        }
        desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(index, key[index].texture));
        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(index + kMaterialSamplerBindingOffset, key[index].nvrhiSampler));
    }
    Entry entry;
    entry.bindingSet = CreateNvrhiBindingSet(m_device, desc, m_layout, "Failed to create a material binding set");
    entry.set = ToNative<VkDescriptorSet>(entry.bindingSet->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSet));
    m_entries.emplace(key, entry);
    m_keyOfSet.emplace(entry.set, key);
    return entry.set;
}

void VulkanMaterialSetCache::Retain(VkDescriptorSet set)
{
    Entry& entry = m_entries.at(m_keyOfSet.at(set));
    ++entry.references;
    entry.pending = false;
}

void VulkanMaterialSetCache::Release(VkDescriptorSet set)
{
    const auto key = m_keyOfSet.find(set);
    if (key == m_keyOfSet.end())
    {
        return;
    }
    Entry& entry = m_entries.at(key->second);
    if (entry.references > 0 && --entry.references == 0)
    {
        m_unreferenced.push_back(set);
    }
}

void VulkanMaterialSetCache::FreeUnreferenced(const std::function<void(std::function<void()>)>& retire)
{
    for (const VkDescriptorSet set : m_unreferenced)
    {
        const auto key = m_keyOfSet.find(set);
        if (key == m_keyOfSet.end())
        {
            continue;
        }
        const auto entry = m_entries.find(key->second);
        if (entry != m_entries.end() && entry->second.references == 0 && !entry->second.pending)
        {
            // The native passes bound the set without NVRHI seeing it, so the frames that may still
            // draw with it keep it alive through retire, not NVRHI's own tracking.
            nvrhi::BindingSetHandle freed = std::move(entry->second.bindingSet);
            m_entries.erase(entry);
            m_keyOfSet.erase(key);
            retire([freed = std::move(freed)]() mutable
                   {
                       freed = nullptr;
                   });
        }
    }
    m_unreferenced.clear();
}

void VulkanMaterialSetCache::AbandonPending()
{
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->second.pending)
        {
            m_keyOfSet.erase(entry->second.set);
            entry = m_entries.erase(entry);
            continue;
        }
        ++entry;
    }
}

size_t VulkanMaterialSetCache::Size() const
{
    return m_entries.size();
}
}
