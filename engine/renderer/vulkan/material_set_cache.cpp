#include "material_set_cache.h"

#include <cstring>
#include <functional>

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

VulkanMaterialSetCache::VulkanMaterialSetCache(VkDevice device, VkDescriptorSetLayout materialSetLayout)
    : m_device(device),
      m_pools(device, materialSetLayout, {VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaterialTextureBindingCount}}, kSetsPerPool)
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

    Entry entry;
    const VulkanDescriptorPoolList::Allocation allocation = m_pools.Allocate();
    entry.set = allocation.set;
    entry.pool = allocation.pool;

    std::array<VkDescriptorImageInfo, kMaterialTextureBindingCount> imageInfos{};
    std::array<VkWriteDescriptorSet, kMaterialTextureBindingCount> writes{};
    for (uint32_t index = 0; index < kMaterialTextureBindingCount; ++index)
    {
        imageInfos[index] = VkDescriptorImageInfo{key[index].sampler, key[index].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = entry.set;
        writes[index].dstBinding = index;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].descriptorCount = 1;
        writes[index].pImageInfo = &imageInfos[index];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    m_entries.emplace(key, entry);
    m_keyOfSet.emplace(entry.set, key);
    return entry.set;
}

void VulkanMaterialSetCache::Free(const Entry& entry)
{
    m_pools.Free(VulkanDescriptorPoolList::Allocation{entry.set, entry.pool});
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

void VulkanMaterialSetCache::FreeUnreferenced()
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
            Free(entry->second);
            m_entries.erase(entry);
            m_keyOfSet.erase(key);
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
            Free(entry->second);
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
