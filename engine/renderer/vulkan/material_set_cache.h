#pragma once

#include "common.h"
#include "descriptor_pool_list.h"
#include "uniform_buffer.h"

#include <array>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me
{

// Set 1, the material descriptor sets, kept from one content upload to the next. A set names a
// material's 32 texture bindings and nothing else, so a material that is still drawn after a change
// keeps its set and only new materials are written: a streamed map changed a cell every few seconds,
// and writing every material's set again each time was the most of a change's cost.
//
// Sets come from pools that free sets one at a time, and a new pool is made when one runs out. A set
// made for an upload is pending until a commit retains it; AbandonPending frees those of an upload that
// failed, so no set outlives textures that never became content.
class VulkanMaterialSetCache
{
  public:
    VulkanMaterialSetCache(VkDevice device, VkDescriptorSetLayout materialSetLayout);
    ~VulkanMaterialSetCache();

    VulkanMaterialSetCache(const VulkanMaterialSetCache&) = delete;
    VulkanMaterialSetCache& operator=(const VulkanMaterialSetCache&) = delete;

    // The set for these bindings: the one already made for them, or one made and written now.
    VkDescriptorSet Acquire(const MaterialTextureBinding& binding);
    // Commit's counts: a draw of the new content takes a reference to its set, a draw it drops gives
    // one back. Then the sets nothing references are freed (the frames that drew them have finished),
    // before the textures they name are destroyed.
    void Retain(VkDescriptorSet set);
    void Release(VkDescriptorSet set);
    void FreeUnreferenced();
    void AbandonPending();
    size_t Size() const;

  private:
    using Key = std::array<TextureDescriptorBinding, kMaterialTextureBindingCount>;
    struct KeyHash
    {
        size_t operator()(const Key& key) const;
    };
    struct KeyEqual
    {
        bool operator()(const Key& a, const Key& b) const;
    };
    struct Entry
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t pool = 0;
        uint32_t references = 0;
        bool pending = true;
    };

    static Key KeyOf(const MaterialTextureBinding& binding);
    void Free(const Entry& entry);

    static constexpr uint32_t kSetsPerPool = 1024;

    VkDevice m_device = VK_NULL_HANDLE;
    VulkanDescriptorPoolList m_pools;
    std::unordered_map<Key, Entry, KeyHash, KeyEqual> m_entries;
    std::unordered_map<VkDescriptorSet, Key> m_keyOfSet;
    // Sets that dropped to no reference since the last FreeUnreferenced.
    std::vector<VkDescriptorSet> m_unreferenced;
};
}
