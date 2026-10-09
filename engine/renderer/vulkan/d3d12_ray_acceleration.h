#pragma once

#include "ray_acceleration.h"

#include <nvrhi/nvrhi.h>

namespace nvrhi::d3d12
{
class IDevice;
}

#include <memory>

namespace me
{

// The ray scene's acceleration structures on Direct3D 12 (D3D12RayAcceleration in the .cpp), built
// natively as the Vulkan ones are: bottom levels suballocated from large buffers (a committed
// resource per structure would cost 64 KiB each, and a streamed map has tens of thousands), compacted
// once their size is known, skinned ones refitted every frame, and a top level per frame slot.
std::unique_ptr<IRayAcceleration> CreateD3D12RayAcceleration(nvrhi::d3d12::IDevice* device, uint32_t frameCount);
}
