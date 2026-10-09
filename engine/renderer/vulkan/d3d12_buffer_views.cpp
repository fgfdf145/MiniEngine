// The Agility SDK's headers before anything includes the Windows SDK's (nvrhi/d3d12.h does).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <directx/d3d12.h>

#include "d3d12_buffer_views.h"

#include <nvrhi/d3d12.h>

#include <atomic>
#include <stdexcept>

namespace me
{

namespace
{
std::atomic<nvrhi::d3d12::IDevice*> g_device{nullptr};
}

void SetD3D12BufferViewDevice(nvrhi::d3d12::IDevice* device)
{
    g_device.store(device);
}

uint32_t CreateD3D12RawBufferView(nvrhi::IBuffer* buffer)
{
    nvrhi::d3d12::IDevice* device = g_device.load();
    if (device == nullptr || buffer == nullptr)
    {
        throw std::runtime_error("No D3D12 device for a mesh buffer view");
    }
    nvrhi::d3d12::IDescriptorHeap* heap = device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView);
    const nvrhi::d3d12::DescriptorIndex index = heap->allocateDescriptor();
    if (index == ~nvrhi::d3d12::DescriptorIndex{0})
    {
        throw std::runtime_error("The D3D12 descriptor heap is full");
    }
    auto* nativeDevice = static_cast<ID3D12Device*>(device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer);
    auto* resource = static_cast<ID3D12Resource*>(buffer->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.FirstElement = 0;
    view.Buffer.NumElements = static_cast<UINT>(buffer->getDesc().byteSize / 4);
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    // NVRHI keeps a CPU copy of its heap and the shader-visible one: both get the view.
    nativeDevice->CreateShaderResourceView(resource, &view, heap->getCpuHandle(index));
    nativeDevice->CreateShaderResourceView(resource, &view, heap->getCpuHandleShaderVisible(index));
    return index;
}

void ReleaseD3D12RawBufferView(uint32_t heapIndex)
{
    nvrhi::d3d12::IDevice* device = g_device.load();
    if (device != nullptr && heapIndex != kNoD3D12BufferView)
    {
        device->getDescriptorHeap(nvrhi::d3d12::DescriptorHeapType::ShaderResourceView)->releaseDescriptor(heapIndex);
    }
}
}
