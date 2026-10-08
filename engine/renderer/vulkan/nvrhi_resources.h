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
}
