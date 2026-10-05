#pragma once

#include "common.h"

#include <cstdint>
#include <vector>

namespace me
{

// Descriptor sets of one layout, allocated and freed one at a time, from as many pools as they need.
// Each pool's sets are counted, so an allocation goes straight to a pool with room: trying full pools
// until one took the set cost a streamed map's change thousands of failed allocations once there were
// a dozen pools.
class VulkanDescriptorPoolList
{
  public:
    struct Allocation
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t pool = 0;
    };

    // perSet: the descriptors of one set; setsPerPool: how many sets a pool holds.
    VulkanDescriptorPoolList(VkDevice device, VkDescriptorSetLayout layout, std::vector<VkDescriptorPoolSize> perSet, uint32_t setsPerPool);
    ~VulkanDescriptorPoolList();

    VulkanDescriptorPoolList(const VulkanDescriptorPoolList&) = delete;
    VulkanDescriptorPoolList& operator=(const VulkanDescriptorPoolList&) = delete;

    Allocation Allocate();
    void Free(const Allocation& allocation);

  private:
    uint32_t CreatePool();

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPoolSize> m_perSet;
    uint32_t m_setsPerPool = 0;
    std::vector<VkDescriptorPool> m_pools;
    std::vector<uint32_t> m_used;
    // Where the last allocation found room; the search starts there.
    uint32_t m_hint = 0;
};
}
