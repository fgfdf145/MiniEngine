// The Direct3D 12 device and DXGI swapchain (docs/design/2026-10-09-d3d12-backend-design.md).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// The Agility SDK's headers before anything includes the Windows SDK's (nvrhi/d3d12.h does).
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "gpu_device.h"

#include "command.h"
#include "d3d12_buffer_views.h"
#include "memory_pool.h"
#include "nvrhi_native.h"
#include "nvrhi_pass.h"

#include <engine/core/log/log.h>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

namespace
{
using Microsoft::WRL::ComPtr;

bool ValidationRequested()
{
    const char* value = std::getenv("MINIENGINE_NVRHI_VALIDATION");
    return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != 0;
}

void Check(HRESULT result, const char* what)
{
    if (FAILED(result))
    {
        char code[16];
        std::snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(what) + " (HRESULT 0x" + code + ")");
    }
}

std::string Narrow(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    if (size > 1)
    {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    }
    return result;
}

class MessageLog final : public nvrhi::IMessageCallback
{
  public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override
    {
        switch (severity)
        {
        case nvrhi::MessageSeverity::Info:
            LOG_INFO("[nvrhi] {}", messageText);
            break;
        case nvrhi::MessageSeverity::Warning:
            LOG_WARN("[nvrhi] {}", messageText);
            break;
        default:
            LOG_ERROR("[nvrhi] {}", messageText);
            break;
        }
    }
};

class D3D12GpuDevice;

// A flip-model swapchain on the window, three images, with its frame latency waitable object: a frame
// starts only when DXGI can take it, so the CPU never queues frames the display then batches (the
// pacing the Vulkan path's HDR swapchain lacked on NVIDIA, which routes it through DXGI anyway).
// HDR10 is R10G10B10A2 in the ST 2084 / Rec.2020 colour space, when the window's display takes it.
class D3D12GpuSwapchain final : public GpuSwapchain
{
  public:
    D3D12GpuSwapchain(
        nvrhi::IDevice* nvrhiDevice, IDXGIFactory6* factory, ID3D12CommandQueue* queue, SDL_Window* window, VkExtent2D extent, bool preferHdr)
        : m_device(nvrhiDevice)
    {
        HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd == nullptr)
        {
            throw std::runtime_error("The window has no HWND for a DXGI swapchain");
        }
        m_extent = {std::max(extent.width, 1u), std::max(extent.height, 1u)};
        m_hdr = preferHdr && DisplaySupportsHdr(factory, hwnd);
        if (preferHdr && !m_hdr)
        {
            LOG_WARN("HDR output was requested, but the window's display is not in HDR mode; presenting SDR");
        }
        const DXGI_FORMAT format = m_hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        m_format = m_hdr ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_extent.width;
        desc.Height = m_extent.height;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        // D3D12 copies to and from any buffer: the clear and MINIENGINE_CAPTURE_WINDOW need no flag.
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kImageCount;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swapchain;
        Check(factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swapchain), "Failed to create the DXGI swapchain");
        Check(swapchain.As(&m_swapchain), "The DXGI swapchain is not an IDXGISwapChain4");
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        // At most the frames in flight queued; Acquire waits for the object to say there is room.
        Check(m_swapchain->SetMaximumFrameLatency(static_cast<UINT>(VulkanCommandContext::kMaxFramesInFlight)), "Failed to set the frame latency");
        m_waitable = m_swapchain->GetFrameLatencyWaitableObject();

        const DXGI_COLOR_SPACE_TYPE colorSpace = m_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        UINT support = 0;
        if (SUCCEEDED(m_swapchain->CheckColorSpaceSupport(colorSpace, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0)
        {
            Check(m_swapchain->SetColorSpace1(colorSpace), "Failed to set the swapchain's colour space");
        }
        else if (m_hdr)
        {
            throw std::runtime_error("The DXGI swapchain cannot present HDR10");
        }

        for (UINT index = 0; index < kImageCount; ++index)
        {
            ComPtr<ID3D12Resource> buffer;
            Check(m_swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)), "Failed to get a swapchain buffer");
            nvrhi::TextureDesc textureDesc;
            textureDesc.width = m_extent.width;
            textureDesc.height = m_extent.height;
            textureDesc.format = ToNvrhiFormat(m_format);
            textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            textureDesc.isRenderTarget = true;
            textureDesc.debugName = "Swapchain image";
            textureDesc.initialState = nvrhi::ResourceStates::Present;
            nvrhi::TextureHandle texture =
                m_device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(buffer.Get()), textureDesc);
            if (!texture)
            {
                throw std::runtime_error("Failed to wrap a swapchain buffer for NVRHI");
            }
            m_images.push_back(std::move(texture));
        }
        LOG_INFO("DXGI swapchain {}x{} {}", m_extent.width, m_extent.height, m_hdr ? "HDR10" : "SDR");
    }

    ~D3D12GpuSwapchain() override
    {
        // NVRHI lets go of the buffers once the GPU is done with them; the swapchain itself must be the
        // last reference before another is made on the window.
        m_device->waitForIdle();
        m_images.clear();
        m_device->runGarbageCollection();
        if (m_waitable != nullptr)
        {
            CloseHandle(m_waitable);
        }
    }

    uint32_t GetImageCount() const override
    {
        return static_cast<uint32_t>(m_images.size());
    }
    nvrhi::ITexture* GetImage(uint32_t index) const override
    {
        return m_images.at(index);
    }
    VkFormat GetFormat() const override
    {
        return m_format;
    }
    VkExtent2D GetExtent() const override
    {
        return m_extent;
    }
    bool IsHdr() const override
    {
        return m_hdr;
    }

    SwapchainStatus Acquire(uint32_t frameSlot, uint32_t& imageIndex) override
    {
        (void)frameSlot;
        // Room in DXGI's queue for this frame (a second at most: an occluded window may never say).
        WaitForSingleObjectEx(m_waitable, 1000, TRUE);
        imageIndex = m_swapchain->GetCurrentBackBufferIndex();
        return SwapchainStatus::Ok;
    }

    void BeforeSubmit(uint32_t, uint32_t) override
    {
        // One queue: the present after the frame's work orders itself.
    }

    SwapchainStatus Present(uint32_t imageIndex) override
    {
        (void)imageIndex;
        // Interval 0 without tearing: the newest frame at each refresh, as Vulkan's mailbox mode.
        const HRESULT result = m_swapchain->Present(0, 0);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        {
            throw std::runtime_error("The D3D12 device was removed while presenting");
        }
        return SUCCEEDED(result) ? SwapchainStatus::Ok : SwapchainStatus::OutOfDate;
    }

  private:
    static constexpr UINT kImageCount = 3;

    static bool DisplaySupportsHdr(IDXGIFactory6* factory, HWND hwnd)
    {
        // The output the window is mostly on, through the adapters' outputs.
        RECT window{};
        GetWindowRect(hwnd, &window);
        ComPtr<IDXGIAdapter1> adapter;
        long bestArea = -1;
        bool hdr = false;
        for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
        {
            ComPtr<IDXGIOutput> output;
            for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex)
            {
                DXGI_OUTPUT_DESC desc{};
                output->GetDesc(&desc);
                const RECT& r = desc.DesktopCoordinates;
                const long width = std::max(0L, std::min(window.right, r.right) - std::max(window.left, r.left));
                const long height = std::max(0L, std::min(window.bottom, r.bottom) - std::max(window.top, r.top));
                if (width * height > bestArea)
                {
                    bestArea = width * height;
                    ComPtr<IDXGIOutput6> output6;
                    DXGI_OUTPUT_DESC1 desc1{};
                    hdr = SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1)) &&
                          desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }
            }
        }
        return hdr;
    }

    nvrhi::IDevice* m_device = nullptr;
    ComPtr<IDXGISwapChain4> m_swapchain;
    HANDLE m_waitable = nullptr;
    std::vector<nvrhi::TextureHandle> m_images;
    VkExtent2D m_extent{};
    VkFormat m_format = VK_FORMAT_B8G8R8A8_UNORM;
    bool m_hdr = false;
};

class D3D12GpuDevice final : public GpuDevice
{
  public:
    explicit D3D12GpuDevice(bool allowRayQuery)
    {
        const bool validation = ValidationRequested();
        if (validation)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                LOG_INFO("D3D12 debug layer on");
            }
        }
        Check(CreateDXGIFactory2(validation ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_factory)), "Failed to create the DXGI factory");

        // The first high-performance hardware adapter that makes a 12_1 device.
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0; m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                             DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device))))
            {
                m_adapter = adapter;
                m_adapterName = Narrow(desc.Description);
                break;
            }
        }
        if (!m_device)
        {
            throw std::runtime_error("No adapter makes a Direct3D 12 (feature level 12_1) device");
        }
        if (validation)
        {
            // The debug layer's messages into the engine log, as the Vulkan validation layer's go.
            ComPtr<ID3D12InfoQueue1> infoQueue;
            if (SUCCEEDED(m_device.As(&infoQueue)))
            {
                DWORD cookie = 0;
                infoQueue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR description, void*)
                    {
                        if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                        {
                            LOG_ERROR("[d3d12] {}", description);
                        }
                        else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
                        {
                            LOG_WARN("[d3d12] {}", description);
                        }
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                    nullptr,
                    &cookie);
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)), "Failed to create the D3D12 queue");
        m_queue->SetName(L"Graphics queue");

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        m_hardwareRayQuery = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        // The ray tracing builds are Vulkan's so far: hardware rays on D3D12 come with its own
        // acceleration structures (RayAccelerationD3D12).
        m_rayQuery = allowRayQuery && m_hardwareRayQuery && D3D12RayTracingImplemented();
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))))
        {
            m_gpuUploadHeap = options16.GPUUploadHeapSupported != FALSE;
        }

        nvrhi::d3d12::DeviceDesc desc;
        desc.errorCB = &m_messageLog;
        desc.pDevice = m_device.Get();
        desc.pGraphicsCommandQueue = m_queue.Get();
        // Material sets of 32 textures each, tens of thousands on a streamed map, and the bindless ray
        // texture table: the shader-visible heap at its largest. Samplers: the frame sets' tables of
        // every material sampler (D3D12's limit is 2048 in the shader-visible heap).
        desc.shaderResourceViewHeapSize = 1000000;
        desc.samplerHeapSize = 2048;
        desc.renderTargetViewHeapSize = 4096;
        desc.depthStencilViewHeapSize = 1024;
        desc.maxTimerQueries = 1024;
        desc.enableHeapDirectlyIndexed = true;
        m_d3d12 = nvrhi::d3d12::createDevice(desc);
        if (!m_d3d12)
        {
            throw std::runtime_error("Failed to create the NVRHI D3D12 device");
        }
        m_nvrhi = m_d3d12;
        SetD3D12BufferViewDevice(m_d3d12.Get());
        if (validation)
        {
            m_nvrhi = nvrhi::validation::createValidationLayer(m_d3d12);
            LOG_INFO("NVRHI device created (D3D12, validation layer on)");
        }
        else
        {
            LOG_INFO("NVRHI device created (D3D12)");
        }
        SetNativeViewportConvention(nvrhi::GraphicsAPI::D3D12);
    }

    ~D3D12GpuDevice() override
    {
        if (m_nvrhi)
        {
            m_nvrhi->waitForIdle();
            m_nvrhi->runGarbageCollection();
            VulkanMemoryPool::UnregisterNvrhiDevice(m_nvrhi.Get());
        }
        SetD3D12BufferViewDevice(nullptr);
        m_nvrhi = nullptr;
        m_d3d12 = nullptr;
    }

    nvrhi::IDevice* Get() const override
    {
        return m_nvrhi.Get();
    }
    std::string GetAdapterName() const override
    {
        return m_adapterName;
    }
    bool SupportsRayQuery() const override
    {
        return m_rayQuery;
    }
    bool SupportsBlockCompression() const override
    {
        return true;
    }
    bool SupportsIndependentBlend() const override
    {
        return true;
    }
    bool SupportsUpdateUnusedWhilePending() const override
    {
        // A D3D12 descriptor heap's entries may be written while the GPU reads others.
        return true;
    }
    bool HasLargeHostVisibleDeviceMemory() const override
    {
        return m_gpuUploadHeap;
    }
    float GetMaxSamplerAnisotropy() const override
    {
        return 16.0f;
    }
    GpuLocalMemory QueryLocalMemory() const override
    {
        GpuLocalMemory local;
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(m_adapter.As(&adapter3)) && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            local.usage = info.CurrentUsage;
            local.budget = info.Budget;
            local.measured = true;
        }
        return local;
    }
    VkExtent2D GetSwapchainExtent(SDL_Window* window) const override
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return VkExtent2D{static_cast<uint32_t>(std::max(width, 1)), static_cast<uint32_t>(std::max(height, 1))};
    }
    std::unique_ptr<GpuSwapchain> CreateSwapchain(SDL_Window* window, bool preferHdr) override
    {
        return std::make_unique<D3D12GpuSwapchain>(m_nvrhi.Get(), m_factory.Get(), m_queue.Get(), window, GetSwapchainExtent(window), preferHdr);
    }
    nvrhi::d3d12::IDevice* GetNvrhiD3D12() const override
    {
        return m_d3d12.Get();
    }

  private:
    static bool D3D12RayTracingImplemented()
    {
        return true;
    }

    MessageLog m_messageLog;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    nvrhi::d3d12::DeviceHandle m_d3d12;
    nvrhi::DeviceHandle m_nvrhi;
    std::string m_adapterName;
    bool m_hardwareRayQuery = false;
    bool m_rayQuery = false;
    bool m_gpuUploadHeap = false;
};
}

std::unique_ptr<GpuDevice> CreateD3D12GpuDevice(bool allowRayQuery)
{
    return std::make_unique<D3D12GpuDevice>(allowRayQuery);
}
}
