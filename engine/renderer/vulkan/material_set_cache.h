#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <array>
#include <cstddef>
#include <functional>
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
// A set is an NVRHI binding set of the material layout, which hands them out from its shared pools
// (VulkanMaterialDescriptorSetLayout). It names textures only: each binding's sampler is an index into
// the frame set's sampler table (GpuMaterialData::samplerIndices), so two materials that read the same
// textures through different samplers share a set. A set made for an upload is pending until a commit
// retains it; AbandonPending frees those of an upload that failed, so no set outlives textures that
// never became content.
class VulkanMaterialSetCache
{
  public:
    VulkanMaterialSetCache(nvrhi::IDevice* device, nvrhi::IBindingLayout* materialSetLayout);
    ~VulkanMaterialSetCache();

    VulkanMaterialSetCache(const VulkanMaterialSetCache&) = delete;
    VulkanMaterialSetCache& operator=(const VulkanMaterialSetCache&) = delete;

    // The set for these bindings: the one already made for them, or one made and written now.
    nvrhi::IBindingSet* Acquire(const MaterialTextureBinding& binding);
    // Commit's counts: a draw of the new content takes a reference to its set, a draw it drops gives
    // one back. Then the sets nothing references leave the cache, and retire frees each once the frames
    // that may still draw with it have finished (before the textures they name are destroyed, which
    // are retired after them).
    void Retain(nvrhi::IBindingSet* set);
    void Release(nvrhi::IBindingSet* set);
    void FreeUnreferenced(const std::function<void(std::function<void()>)>& retire);
    void AbandonPending();
    size_t Size() const;

  private:
    using Key = std::array<nvrhi::ITexture*, kMaterialTextureBindingCount>;
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
        nvrhi::BindingSetHandle bindingSet;
        uint32_t references = 0;
        bool pending = true;
    };

    static Key KeyOf(const MaterialTextureBinding& binding);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::IBindingLayout* m_layout = nullptr;
    std::unordered_map<Key, Entry, KeyHash, KeyEqual> m_entries;
    std::unordered_map<nvrhi::IBindingSet*, Key> m_keyOfSet;
    // Sets that dropped to no reference since the last FreeUnreferenced.
    std::vector<nvrhi::IBindingSet*> m_unreferenced;
};
}
