#include "memory_pool.h"

#include <engine/renderer/block_suballocator.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace me
{

struct VulkanMemoryBlock
{
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t memoryTypeIndex = 0;
    VulkanMemoryPool::Resource resource = VulkanMemoryPool::Resource::Buffer;
    BlockSuballocator ranges;

    explicit VulkanMemoryBlock(VkDeviceSize size)
        : ranges(size)
    {
    }
};

namespace
{
constexpr VkDeviceSize kBlockSize = VkDeviceSize{64} << 20;
constexpr VkDeviceSize kDedicatedThreshold = kBlockSize / 4;

std::mutex g_mutex;
std::vector<std::unique_ptr<VulkanMemoryBlock>> g_blocks;

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("Failed to find a suitable Vulkan memory type");
}

VkDeviceMemory AllocateMemory(VkDevice device, VkDeviceSize size, uint32_t memoryTypeIndex)
{
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    CheckVulkan(vkAllocateMemory(device, &allocateInfo, nullptr, &memory), "Failed to allocate Vulkan device memory");
    return memory;
}
}

VulkanPooledMemory VulkanMemoryPool::Allocate(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags properties,
    Resource resource)
{
    const uint32_t memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, properties);

    VulkanPooledMemory result{};
    result.size = requirements.size;
    if (requirements.size >= kDedicatedThreshold)
    {
        result.memory = AllocateMemory(device, requirements.size, memoryTypeIndex);
        return result;
    }

    std::lock_guard lock(g_mutex);
    for (const std::unique_ptr<VulkanMemoryBlock>& block : g_blocks)
    {
        if (block->device != device || block->memoryTypeIndex != memoryTypeIndex || block->resource != resource)
        {
            continue;
        }
        if (const std::optional<uint64_t> offset = block->ranges.Allocate(requirements.size, requirements.alignment))
        {
            result.memory = block->memory;
            result.offset = *offset;
            result.block = block.get();
            return result;
        }
    }

    auto block = std::make_unique<VulkanMemoryBlock>(kBlockSize);
    block->device = device;
    block->memoryTypeIndex = memoryTypeIndex;
    block->resource = resource;
    block->memory = AllocateMemory(device, kBlockSize, memoryTypeIndex);
    const std::optional<uint64_t> offset = block->ranges.Allocate(requirements.size, requirements.alignment);
    if (!offset)
    {
        vkFreeMemory(device, block->memory, nullptr);
        throw std::runtime_error("Vulkan memory pool: a request does not fit in a fresh block");
    }
    result.memory = block->memory;
    result.offset = *offset;
    result.block = block.get();
    g_blocks.push_back(std::move(block));
    return result;
}

void VulkanMemoryPool::Free(VkDevice device, VulkanPooledMemory& allocation)
{
    if (allocation.memory == VK_NULL_HANDLE)
    {
        return;
    }
    if (allocation.block == nullptr)
    {
        vkFreeMemory(device, allocation.memory, nullptr);
        allocation = {};
        return;
    }

    std::lock_guard lock(g_mutex);
    VulkanMemoryBlock* block = allocation.block;
    block->ranges.Free(allocation.offset, allocation.size);
    if (block->ranges.Empty())
    {
        vkFreeMemory(block->device, block->memory, nullptr);
        g_blocks.erase(std::find_if(
            g_blocks.begin(),
            g_blocks.end(),
            [block](const std::unique_ptr<VulkanMemoryBlock>& candidate)
            {
                return candidate.get() == block;
            }));
    }
    allocation = {};
}
}
