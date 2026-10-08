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
// apart without having to honour bufferImageGranularity; buffers whose device address shaders or
// acceleration structure builds use come from blocks allocated with the device address flag. Thread-safe. A block whose last range
// is freed stays for the next allocations: vkFreeMemory of a block took about a millisecond of the
// driver's lock, and bottom-level compaction emptied dozens of blocks a frame while filling new ones.
// ReleaseEmptyBlocks gives the spares back a few at a time, and all of them before the device goes.
namespace VulkanMemoryPool
{
enum class Resource
{
    Buffer,
    Image,
    // A buffer created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (needs bufferDeviceAddress).
    AddressableBuffer,
};

VulkanPooledMemory Allocate(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags properties,
    Resource resource);

// Releases the range and resets it to empty. Safe on an empty range. A dedicated allocation is freed
// now; a block left empty is kept (ReleaseEmptyBlocks).
void Free(VkDevice device, VulkanPooledMemory& allocation);

// Frees up to maxRelease of the device's empty blocks beyond the first keep of them; returns how many
// it freed. Once a frame with a small maxRelease, and with keep 0 and no limit before the device is
// destroyed.
size_t ReleaseEmptyBlocks(VkDevice device, size_t keep, size_t maxRelease);

// The device memory the pool holds from the driver (whole blocks and dedicated allocations): the
// scene's buffers, textures and acceleration structures, which is what world streaming budgets.
uint64_t CommittedBytes();
}
}
