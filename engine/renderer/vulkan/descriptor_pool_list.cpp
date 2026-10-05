#include "descriptor_pool_list.h"

namespace me
{

VulkanDescriptorPoolList::VulkanDescriptorPoolList(
    VkDevice device,
    VkDescriptorSetLayout layout,
    std::vector<VkDescriptorPoolSize> perSet,
    uint32_t setsPerPool)
    : m_device(device),
      m_layout(layout),
      m_perSet(std::move(perSet)),
      m_setsPerPool(setsPerPool)
{
}

VulkanDescriptorPoolList::~VulkanDescriptorPoolList()
{
    for (VkDescriptorPool pool : m_pools)
    {
        vkDestroyDescriptorPool(m_device, pool, nullptr);
    }
}

uint32_t VulkanDescriptorPoolList::CreatePool()
{
    std::vector<VkDescriptorPoolSize> sizes = m_perSet;
    for (VkDescriptorPoolSize& size : sizes)
    {
        size.descriptorCount *= m_setsPerPool;
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = m_setsPerPool;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &pool), "Failed to create a descriptor pool");
    m_pools.push_back(pool);
    m_used.push_back(0);
    return static_cast<uint32_t>(m_pools.size() - 1);
}

VulkanDescriptorPoolList::Allocation VulkanDescriptorPoolList::Allocate()
{
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_layout;
    const uint32_t poolCount = static_cast<uint32_t>(m_pools.size());
    for (uint32_t step = 0; step < poolCount; ++step)
    {
        const uint32_t index = (m_hint + step) % poolCount;
        if (m_used[index] >= m_setsPerPool)
        {
            continue;
        }
        allocateInfo.descriptorPool = m_pools[index];
        Allocation allocation;
        // A pool with room by count can still be fragmented; it is passed over then.
        if (vkAllocateDescriptorSets(m_device, &allocateInfo, &allocation.set) == VK_SUCCESS)
        {
            allocation.pool = index;
            ++m_used[index];
            m_hint = index;
            return allocation;
        }
    }
    Allocation allocation;
    allocation.pool = CreatePool();
    allocateInfo.descriptorPool = m_pools[allocation.pool];
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, &allocation.set), "Failed to allocate a descriptor set");
    ++m_used[allocation.pool];
    m_hint = allocation.pool;
    return allocation;
}

void VulkanDescriptorPoolList::Free(const Allocation& allocation)
{
    if (allocation.set == VK_NULL_HANDLE || allocation.pool >= m_pools.size())
    {
        return;
    }
    vkFreeDescriptorSets(m_device, m_pools[allocation.pool], 1, &allocation.set);
    --m_used[allocation.pool];
}
}
