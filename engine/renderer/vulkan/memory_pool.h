#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>

namespace me
{

struct VulkanMemoryBlock;

// A range of device memory from VulkanMemoryPool, in an NVRHI heap: an NVRHI resource made virtual
// binds at (heap, offset) with bindBufferMemory/bindTextureMemory; on Vulkan a native one binds at
// (memory, offset), memory being the heap's own VkDeviceMemory (null on D3D12).
struct VulkanPooledMemory
{
    nvrhi::HeapHandle heap;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    VulkanMemoryBlock* block = nullptr; // null for a dedicated allocation, whose heap is its own
};

// Shares a few large device-local allocations (NVRHI heaps) between scene buffers and textures. Every
// vkAllocateMemory costs the driver tens of kilobytes however small it is: a map with 40,000
// submeshes (three buffers each) and 7,000 textures ran an 8 GB GPU out of memory with about 1 GB
// actually requested. Blocks are 64 MiB; a request of 16 MiB or more gets its own allocation.
// Buffers and images come from separate blocks, which keeps linear and optimal-tiling resources
// apart without having to honour bufferImageGranularity; buffers whose device address shaders or
// acceleration structure builds use come from blocks allocated with the device address flag. Thread-safe. A block whose last range
// is freed stays for the next allocations: vkFreeMemory of a block took about a millisecond of the
// driver's lock, and bottom-level compaction emptied dozens of blocks a frame while filling new ones.
// ReleaseEmptyBlocks gives the spares back a few at a time, and UnregisterNvrhiDevice all of them before
// the device goes. The blocks are NVRHI heaps of either backend; on D3D12 they hold buffers and
// textures alike (D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES), still kept apart by kind.
namespace VulkanMemoryPool
{
enum class Resource
{
    Buffer,
    Image,
    // A buffer created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (needs bufferDeviceAddress).
    AddressableBuffer,
};

// The NVRHI device whose heaps a native Vulkan allocation (Allocate's VkDevice overload) comes from.
// NvrhiDevice registers itself and unregisters before it is destroyed: Unregister releases every
// block of the device, all of which must be empty by then (a block still in use is leaked, and
// logged).
void RegisterNvrhiDevice(VkDevice device, nvrhi::IDevice* nvrhiDevice);
void UnregisterNvrhiDevice(nvrhi::IDevice* nvrhiDevice);

// Memory for a virtual NVRHI buffer or texture made on device, which the caller then binds at
// (heap, offset). Out of device memory throws a VulkanError that IsOutOfMemory.
VulkanPooledMemory AllocateFor(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, Resource resource);
VulkanPooledMemory AllocateFor(nvrhi::IDevice* device, nvrhi::ITexture* texture);

// The same for a native Vulkan resource (the acceleration structures): the heap's memory type is the
// first one in requirements.memoryTypeBits with every one of properties (device local only).
VulkanPooledMemory Allocate(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags properties,
    Resource resource);

// Releases the range and resets it to empty. Safe on an empty range. A dedicated allocation is freed
// now; a block left empty is kept (ReleaseEmptyBlocks).
void Free(VulkanPooledMemory& allocation);
inline void Free(VkDevice /*device*/, VulkanPooledMemory& allocation)
{
    Free(allocation);
}

// Frees up to maxRelease of the device's empty blocks beyond the first keep of them; returns how many
// it freed. Once a frame with a small maxRelease, and with keep 0 and no limit before the device is
// destroyed.
size_t ReleaseEmptyBlocks(nvrhi::IDevice* device, size_t keep, size_t maxRelease);

// The device memory the pool holds from the driver (whole blocks and dedicated allocations): the
// scene's buffers, textures and acceleration structures, which is what world streaming budgets.
uint64_t CommittedBytes();
}
}
