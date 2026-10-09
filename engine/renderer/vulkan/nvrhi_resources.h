#pragma once

#include "common.h"
#include "nvrhi_native.h"

namespace me
{

// The NVRHI texture for an image the native code describes (docs/design/2026-10-08-nvrhi-backend-design.md,
// stage A3): its format, extent, levels and layers, 1D, 2D, 3D or cube, usage and MUTABLE_FORMAT,
// as NVRHI's TextureDesc says them. NVRHI gives it memory of its own, device local, as each of these
// images had, and adds both transfer usages. A description NVRHI cannot say (another tiling, sharing
// mode or create flag) throws rather than make a different image. image gets the VkImage. Out of
// device memory throws a VulkanError that IsOutOfMemory, as vkCreateImage and vkAllocateMemory did.
nvrhi::TextureHandle CreateNvrhiImage(nvrhi::IDevice* device, const VkImageCreateInfo& info, VkImage& image, const char* failureMessage);

// The NVRHI buffer for a buffer the native code describes (stage A4): its size and usage (vertex,
// index, uniform, storage, indirect, acceleration structure input or storage, shader binding table),
// with memory of its own of one of the kinds the native code allocated: device local
// (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), host visible and coherent (NVRHI's write access: the first
// host-visible type, coherent on every desktop driver) or host cached (its read access). NVRHI adds
// both transfer usages, and the device address usage when the device has buffer device addresses.
// buffer gets the VkBuffer; mapped, when given, the host-visible memory mapped for good (unmapped as
// the buffer goes). Anything else throws, as CreateNvrhiImage; so does running out of memory, as a
// VulkanError that IsOutOfMemory.
nvrhi::BufferHandle CreateNvrhiBuffer(
    nvrhi::IDevice* device,
    const VkBufferCreateInfo& info,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    const char* failureMessage,
    void** mapped = nullptr,
    // The element size the shaders declare it with (StructuredBuffer<T>), which D3D12's views need.
    uint32_t structStride = 0);
}
