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
    nvrhi::IDevice* device = nullptr;
    nvrhi::HeapHandle heap;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // The Vulkan memory types the block's heap may be (VkMemoryRequirements::memoryTypeBits); ~0 on
    // D3D12.
    uint32_t memoryTypeBits = ~0u;
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

bool IsVulkan(nvrhi::IDevice* device)
{
    return device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
}

// Called with g_mutex held. A device-local heap; on Vulkan of one of memoryTypeBits' types, which
// NVRHI allocates with the device address flag when the device has buffer device addresses (the
// buffers it makes all ask for them).
nvrhi::HeapHandle CreateHeap(nvrhi::IDevice* device, VkDeviceSize size, uint32_t memoryTypeBits)
{
    nvrhi::HeapDesc desc;
    desc.capacity = size;
    desc.type = nvrhi::HeapType::DeviceLocal;
    desc.memoryTypeBits = memoryTypeBits;
    desc.debugName = "VulkanMemoryPool";
    nvrhi::HeapHandle heap = device->createHeap(desc);
    if (!heap)
    {
        // NVRHI logs the API's result; running out of memory is what it is in practice, and what
        // callers recover from.
        throw VulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "Failed to allocate device memory");
    }
    g_committedBytes += size;
    return heap;
}

VkDeviceMemory NativeMemory(nvrhi::IDevice* device, nvrhi::IHeap* heap)
{
    return IsVulkan(device) ? ToNative<VkDeviceMemory>(heap->getNativeObject(nvrhi::ObjectTypes::VK_DeviceMemory)) : VK_NULL_HANDLE;
}

VulkanPooledMemory AllocateRange(
    nvrhi::IDevice* device, VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeBits, VulkanMemoryPool::Resource resource)
{
    VulkanPooledMemory result{};
    result.size = size;
    std::lock_guard lock(g_mutex);
    if (size >= kDedicatedThreshold)
    {
        result.heap = CreateHeap(device, size, memoryTypeBits);
        result.memory = NativeMemory(device, result.heap);
        return result;
    }

    for (const std::unique_ptr<VulkanMemoryBlock>& block : g_blocks)
    {
        if (block->device != device || block->memoryTypeBits != memoryTypeBits || block->resource != resource)
        {
            continue;
        }
        if (const std::optional<uint64_t> offset = block->ranges.Allocate(size, alignment))
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
    block->memoryTypeBits = memoryTypeBits;
    block->resource = resource;
    block->heap = CreateHeap(device, kBlockSize, memoryTypeBits);
    block->memory = NativeMemory(device, block->heap);
    const std::optional<uint64_t> offset = block->ranges.Allocate(size, alignment);
    if (!offset)
    {
        block->heap = nullptr;
        g_committedBytes -= kBlockSize;
        throw std::runtime_error("Memory pool: a request does not fit in a fresh block");
    }
    result.heap = block->heap;
    result.memory = block->memory;
    result.offset = *offset;
    result.block = block.get();
    g_blocks.push_back(std::move(block));
    return result;
}

VkDevice NativeDevice(nvrhi::IDevice* device)
{
    return ToNative<VkDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_Device));
}
}

void VulkanMemoryPool::RegisterNvrhiDevice(VkDevice device, nvrhi::IDevice* nvrhiDevice)
{
    std::lock_guard lock(g_mutex);
    g_nvrhiDevices[device] = nvrhiDevice;
}

void VulkanMemoryPool::UnregisterNvrhiDevice(nvrhi::IDevice* nvrhiDevice)
{
    std::vector<std::unique_ptr<VulkanMemoryBlock>> released;
    {
        std::lock_guard lock(g_mutex);
        for (auto block = g_blocks.begin(); block != g_blocks.end();)
        {
            if ((*block)->device != nvrhiDevice)
            {
                ++block;
                continue;
            }
            released.push_back(std::move(*block));
            block = g_blocks.erase(block);
        }
        for (auto entry = g_nvrhiDevices.begin(); entry != g_nvrhiDevices.end();)
        {
            entry = entry->second == nvrhiDevice ? g_nvrhiDevices.erase(entry) : std::next(entry);
        }
    }
    for (std::unique_ptr<VulkanMemoryBlock>& block : released)
    {
        if (!block->ranges.Empty())
        {
            // Something still binds to it, and may free its range later: freeing the memory under it,
            // or the block its range names, would be worse than the leak.
            LOG_ERROR("Memory pool: a block is still in use as its device goes; leaking it");
            block->heap.Detach();
            static_cast<void>(block.release());
            continue;
        }
        block->heap = nullptr;
        g_committedBytes -= kBlockSize;
    }
}

VulkanPooledMemory VulkanMemoryPool::AllocateFor(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, Resource resource)
{
    if (IsVulkan(device))
    {
        // The memory types too, which NVRHI's requirements leave out.
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(
            NativeDevice(device), ToNative<VkBuffer>(buffer->getNativeObject(nvrhi::ObjectTypes::VK_Buffer)), &requirements);
        return AllocateRange(device, requirements.size, requirements.alignment, requirements.memoryTypeBits, resource);
    }
    const nvrhi::MemoryRequirements requirements = device->getBufferMemoryRequirements(buffer);
    return AllocateRange(device, requirements.size, requirements.alignment, ~0u, resource);
}

VulkanPooledMemory VulkanMemoryPool::AllocateFor(nvrhi::IDevice* device, nvrhi::ITexture* texture)
{
    if (IsVulkan(device))
    {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(
            NativeDevice(device), ToNative<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image)), &requirements);
        return AllocateRange(device, requirements.size, requirements.alignment, requirements.memoryTypeBits, Resource::Image);
    }
    const nvrhi::MemoryRequirements requirements = device->getTextureMemoryRequirements(texture);
    return AllocateRange(device, requirements.size, requirements.alignment, ~0u, Resource::Image);
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
    nvrhi::IDevice* nvrhiDevice = nullptr;
    {
        std::lock_guard lock(g_mutex);
        const auto found = g_nvrhiDevices.find(device);
        if (found == g_nvrhiDevices.end())
        {
            throw std::runtime_error("Vulkan memory pool: no NVRHI device for this VkDevice");
        }
        nvrhiDevice = found->second;
    }
    return AllocateRange(nvrhiDevice, requirements.size, requirements.alignment, 1u << memoryTypeIndex, resource);
}

void VulkanMemoryPool::Free(VulkanPooledMemory& allocation)
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

size_t VulkanMemoryPool::ReleaseEmptyBlocks(nvrhi::IDevice* device, size_t keep, size_t maxRelease)
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
