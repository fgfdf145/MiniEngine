#include "memory_pool.h"

#include "nvrhi_native.h"

#include <engine/core/log/log.h>
#include <engine/renderer/block_suballocator.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace me
{

struct VulkanMemoryBlock
{
    VkDevice device = VK_NULL_HANDLE;
    nvrhi::HeapHandle heap;
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
std::map<VkDevice, nvrhi::IDevice*> g_nvrhiDevices;
// What the heaps hold from the driver: whole blocks and dedicated allocations.
std::atomic<uint64_t> g_committedBytes{0};

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

// Called with g_mutex held. A device-local heap of exactly memoryTypeIndex; NVRHI allocates it with
// the device address flag when the device has buffer device addresses, which the buffers it makes
// all ask for.
nvrhi::HeapHandle CreateHeap(VkDevice device, VkDeviceSize size, uint32_t memoryTypeIndex)
{
    const auto found = g_nvrhiDevices.find(device);
    if (found == g_nvrhiDevices.end())
    {
        throw std::runtime_error("Vulkan memory pool: no NVRHI device for this VkDevice");
    }
    nvrhi::HeapDesc desc;
    desc.capacity = size;
    desc.type = nvrhi::HeapType::DeviceLocal;
    desc.memoryTypeBits = 1u << memoryTypeIndex;
    desc.debugName = "VulkanMemoryPool";
    nvrhi::HeapHandle heap = found->second->createHeap(desc);
    if (!heap)
    {
        // NVRHI logs the VkResult; running out of memory is what it is in practice, and what
        // callers recover from.
        throw VulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "Failed to allocate Vulkan device memory");
    }
    g_committedBytes += size;
    return heap;
}

VkDeviceMemory NativeMemory(nvrhi::IHeap* heap)
{
    return ToNative<VkDeviceMemory>(heap->getNativeObject(nvrhi::ObjectTypes::VK_DeviceMemory));
}
}

void VulkanMemoryPool::RegisterNvrhiDevice(VkDevice device, nvrhi::IDevice* nvrhiDevice)
{
    std::lock_guard lock(g_mutex);
    g_nvrhiDevices[device] = nvrhiDevice;
}

void VulkanMemoryPool::UnregisterNvrhiDevice(VkDevice device)
{
    std::vector<std::unique_ptr<VulkanMemoryBlock>> released;
    {
        std::lock_guard lock(g_mutex);
        for (auto block = g_blocks.begin(); block != g_blocks.end();)
        {
            if ((*block)->device != device)
            {
                ++block;
                continue;
            }
            released.push_back(std::move(*block));
            block = g_blocks.erase(block);
        }
        g_nvrhiDevices.erase(device);
    }
    for (std::unique_ptr<VulkanMemoryBlock>& block : released)
    {
        if (!block->ranges.Empty())
        {
            // Something still binds to it, and may free its range later: freeing the memory under it,
            // or the block its range names, would be worse than the leak.
            LOG_ERROR("Vulkan memory pool: a block is still in use as its device goes; leaking it");
            block->heap.Detach();
            static_cast<void>(block.release());
            continue;
        }
        block->heap = nullptr;
        g_committedBytes -= kBlockSize;
    }
}

VulkanPooledMemory VulkanMemoryPool::Allocate(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags properties,
    Resource resource)
{
    if (properties != VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    {
        throw std::runtime_error("Vulkan memory pool: only device-local memory is pooled");
    }
    const uint32_t memoryTypeIndex = FindMemoryType(physicalDevice, requirements.memoryTypeBits, properties);

    VulkanPooledMemory result{};
    result.size = requirements.size;
    std::lock_guard lock(g_mutex);
    if (requirements.size >= kDedicatedThreshold)
    {
        result.heap = CreateHeap(device, requirements.size, memoryTypeIndex);
        result.memory = NativeMemory(result.heap);
        return result;
    }

    for (const std::unique_ptr<VulkanMemoryBlock>& block : g_blocks)
    {
        if (block->device != device || block->memoryTypeIndex != memoryTypeIndex || block->resource != resource)
        {
            continue;
        }
        if (const std::optional<uint64_t> offset = block->ranges.Allocate(requirements.size, requirements.alignment))
        {
            result.heap = block->heap;
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
    block->heap = CreateHeap(device, kBlockSize, memoryTypeIndex);
    block->memory = NativeMemory(block->heap);
    const std::optional<uint64_t> offset = block->ranges.Allocate(requirements.size, requirements.alignment);
    if (!offset)
    {
        block->heap = nullptr;
        g_committedBytes -= kBlockSize;
        throw std::runtime_error("Vulkan memory pool: a request does not fit in a fresh block");
    }
    result.heap = block->heap;
    result.memory = block->memory;
    result.offset = *offset;
    result.block = block.get();
    g_blocks.push_back(std::move(block));
    return result;
}

void VulkanMemoryPool::Free(VkDevice /*device*/, VulkanPooledMemory& allocation)
{
    if (!allocation.heap)
    {
        return;
    }
    if (allocation.block == nullptr)
    {
        // The last reference to its own heap, which frees the memory.
        g_committedBytes -= allocation.size;
        allocation = {};
        return;
    }

    std::lock_guard lock(g_mutex);
    allocation.block->ranges.Free(allocation.offset, allocation.size);
    allocation = {};
}

size_t VulkanMemoryPool::ReleaseEmptyBlocks(VkDevice device, size_t keep, size_t maxRelease)
{
    std::vector<std::unique_ptr<VulkanMemoryBlock>> released;
    {
        std::lock_guard lock(g_mutex);
        size_t empty = 0;
        for (auto block = g_blocks.begin(); block != g_blocks.end() && released.size() < maxRelease;)
        {
            if ((*block)->device != device || !(*block)->ranges.Empty() || ++empty <= keep)
            {
                ++block;
                continue;
            }
            released.push_back(std::move(*block));
            block = g_blocks.erase(block);
        }
    }
    // Outside the pool's lock: the workers' allocations do not wait on the driver.
    for (std::unique_ptr<VulkanMemoryBlock>& block : released)
    {
        block->heap = nullptr;
        g_committedBytes -= kBlockSize;
    }
    return released.size();
}

uint64_t VulkanMemoryPool::CommittedBytes()
{
    return g_committedBytes.load();
}
}
