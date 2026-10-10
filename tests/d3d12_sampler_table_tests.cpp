// The D3D12 backend's sampler tables (cmake/vcpkg-overlay-ports/nvrhi/d3d12-shared-sampler-tables.patch)
// on the WARP adapter: binding sets naming the same samplers share one table in the 2048-descriptor
// shader-visible sampler heap, and a full heap fails the binding set instead of writing past the heap
// (the access violation that opening a second scene view crashed on).

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvrhi/d3d12.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
// The engine's frame sets name every material sampler (kMaterialSamplerCount).
constexpr uint32_t kTableSize = 108;
constexpr uint32_t kSamplerHeapSize = 2048;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class CountingCallback final : public nvrhi::IMessageCallback
{
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override
    {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
        {
            ++errors;
        }
        (void)messageText;
    }

    int errors = 0;
};

struct WarpDevice
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    nvrhi::DeviceHandle nvrhi;
};

WarpDevice CreateWarpDevice(CountingCallback& callback)
{
    WarpDevice result;
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "CreateDXGIFactory2 failed");
    ComPtr<IDXGIAdapter> adapter;
    Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "No WARP adapter");
    Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&result.device))), "D3D12CreateDevice failed");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Require(SUCCEEDED(result.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&result.queue))), "CreateCommandQueue failed");

    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = &callback;
    desc.pDevice = result.device.Get();
    desc.pGraphicsCommandQueue = result.queue.Get();
    desc.samplerHeapSize = kSamplerHeapSize;
    result.nvrhi = nvrhi::d3d12::createDevice(desc);
    Require(result.nvrhi != nullptr, "nvrhi::d3d12::createDevice failed");
    return result;
}

nvrhi::BindingLayoutHandle CreateTableLayout(nvrhi::IDevice* device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::All;
    desc.bindings = {nvrhi::BindingLayoutItem::Sampler(0).setSize(kTableSize)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(desc);
    Require(layout != nullptr, "createBindingLayout failed");
    return layout;
}

nvrhi::BindingSetHandle CreateTableSet(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout, nvrhi::ISampler* sampler)
{
    nvrhi::BindingSetDesc desc;
    for (uint32_t index = 0; index < kTableSize; ++index)
    {
        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, sampler).setArrayElement(index));
    }
    return device->createBindingSet(desc, layout);
}

nvrhi::SamplerHandle CreateSampler(nvrhi::IDevice* device, float mipBias)
{
    nvrhi::SamplerDesc desc;
    desc.mipBias = mipBias;
    nvrhi::SamplerHandle sampler = device->createSampler(desc);
    Require(sampler != nullptr, "createSampler failed");
    return sampler;
}

// Far more identical tables than the heap holds (a frame set per frame slot and scene view).
void IdenticalTablesShareOneTable()
{
    CountingCallback callback;
    WarpDevice warp = CreateWarpDevice(callback);
    nvrhi::BindingLayoutHandle layout = CreateTableLayout(warp.nvrhi);
    nvrhi::SamplerHandle sampler = CreateSampler(warp.nvrhi, 0.0f);

    // One sampler object per set too: sharing goes by what the descriptors hold.
    std::vector<nvrhi::SamplerHandle> samplers;
    std::vector<nvrhi::BindingSetHandle> sets;
    for (uint32_t index = 0; index < 4 * kSamplerHeapSize / kTableSize; ++index)
    {
        samplers.push_back(index % 2 == 0 ? sampler : CreateSampler(warp.nvrhi, 0.0f));
        sets.push_back(CreateTableSet(warp.nvrhi, layout, samplers.back()));
        Require(sets.back() != nullptr, "Identical sampler table " + std::to_string(index) + " failed");
    }
    Require(callback.errors == 0, "Identical sampler tables reported errors");

    // The tables a released set held come back to the heap.
    sets.clear();
    std::vector<nvrhi::BindingSetHandle> distinct;
    for (uint32_t index = 0; index < kSamplerHeapSize / kTableSize; ++index)
    {
        distinct.push_back(CreateTableSet(warp.nvrhi, layout, CreateSampler(warp.nvrhi, static_cast<float>(index) * 0.01f)));
        Require(distinct.back() != nullptr, "Distinct sampler table " + std::to_string(index) + " failed after the shared one was released");
    }
}

// More distinct tables than the heap holds: the set that does not fit fails, nothing is written past the heap.
void FullHeapFailsTheBindingSet()
{
    CountingCallback callback;
    WarpDevice warp = CreateWarpDevice(callback);
    nvrhi::BindingLayoutHandle layout = CreateTableLayout(warp.nvrhi);

    std::vector<nvrhi::BindingSetHandle> sets;
    bool failed = false;
    for (uint32_t index = 0; index <= kSamplerHeapSize / kTableSize; ++index)
    {
        nvrhi::BindingSetHandle set = CreateTableSet(warp.nvrhi, layout, CreateSampler(warp.nvrhi, static_cast<float>(index) * 0.01f));
        if (!set)
        {
            failed = true;
            break;
        }
        sets.push_back(set);
    }
    Require(failed, "A sampler table past the heap's 2048 descriptors was created");
    Require(sets.size() == kSamplerHeapSize / kTableSize, "Fewer sampler tables fit than the heap holds");
    Require(callback.errors > 0, "The full sampler heap was not reported");

    // A set already in the heap's tables still comes from it.
    Require(CreateTableSet(warp.nvrhi, layout, CreateSampler(warp.nvrhi, 0.0f)) != nullptr, "A shared table failed on a full heap");
}
}

int main()
{
    try
    {
        IdenticalTablesShareOneTable();
        FullHeapFailsTheBindingSet();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "D3D12 sampler table tests passed\n";
    return 0;
}
