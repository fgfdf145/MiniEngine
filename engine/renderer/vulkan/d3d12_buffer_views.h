#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace nvrhi::d3d12
{
class IDevice;
}

namespace me
{

// Raw views of mesh buffers in NVRHI's shader-visible descriptor heap, for the hit shading on
// Direct3D 12 (RayMeshGeometry and RayHeapBuffer in ray_hit_common.slang): DXIL has no pointers, so a
// mesh's vertices and indices are read by their view's heap index where Vulkan reads device addresses.
// The D3D12 device registers itself while it lives; without one the calls fail (create) or do nothing.
void SetD3D12BufferViewDevice(nvrhi::d3d12::IDevice* device);
// The view's index in the heap. Throws when there is no device or the heap is full.
uint32_t CreateD3D12RawBufferView(nvrhi::IBuffer* buffer);
void ReleaseD3D12RawBufferView(uint32_t heapIndex);

inline constexpr uint32_t kNoD3D12BufferView = ~uint32_t{0};
}
