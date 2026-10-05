#pragma once

#include "common.h"

namespace me
{

struct VulkanMemoryBlock;

// A range of device memory from VulkanMemoryPool: bind the resource at (memory, offset).
struct VulkanPooledMemory
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    VulkanMemoryBlock* block = nullptr; // null for a dedicated allocation
};

// Shares a few large device-local allocations between scene buffers and textures. Every
// vkAllocateMemory costs the driver tens of kilobytes however small it is: a map with 40,000
// submeshes (three buffers each) and 7,000 textures ran an 8 GB GPU out of memory with about 1 GB
// actually requested. Blocks are 64 MiB; a request of 16 MiB or more gets its own allocation.
// Buffers and images come from separate blocks, which keeps linear and optimal-tiling resources
// apart without having to honour bufferImageGranularity. Thread-safe. A block is freed as soon as
// its last range is, so nothing is left to release before the device is destroyed.
namespace VulkanMemoryPool
{
enum class Resource
{
    Buffer,
    Image,
};

VulkanPooledMemory Allocate(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags properties,
    Resource resource);

// Releases the range and resets it to empty. Safe on an empty range.
void Free(VkDevice device, VulkanPooledMemory& allocation);
}
}
