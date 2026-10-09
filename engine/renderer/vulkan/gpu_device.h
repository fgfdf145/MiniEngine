#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>

struct SDL_Window;

namespace nvrhi::vulkan
{
class IDevice;
}
namespace nvrhi::d3d12
{
class IDevice;
}

namespace me
{

class VulkanInstance;
class VulkanDevice;

// The device-local memory's usage and budget summed, in bytes. measured is false when the API gave
// no usage (the budget is then a share of the heaps' size).
struct GpuLocalMemory
{
    uint64_t usage = 0;
    uint64_t budget = 0;
    bool measured = false;
};

enum class SwapchainStatus
{
    Ok,
    // Presentable, but the window and the swapchain no longer match: rebuild before the next frame.
    Suboptimal,
    OutOfDate
};

// The window's swapchain on one graphics API (docs/design/2026-10-09-d3d12-backend-design.md): its
// images as NVRHI textures, and the frame's acquire, submit and present.
class GpuSwapchain
{
  public:
    virtual ~GpuSwapchain() = default;

    virtual uint32_t GetImageCount() const = 0;
    virtual nvrhi::ITexture* GetImage(uint32_t index) const = 0;
    // Vulkan's name for the format, which the scene targets' choices are made in.
    virtual VkFormat GetFormat() const = 0;
    virtual VkExtent2D GetExtent() const = 0;
    // HDR10: 10-bit, PQ-encoded Rec.2020 (hdr_output.slang).
    virtual bool IsHdr() const = 0;

    // The image the frame of frameSlot draws into. The frame slot's previous frame has finished.
    virtual SwapchainStatus Acquire(uint32_t frameSlot, uint32_t& imageIndex) = 0;
    // What the frame's command list must wait for and signal, set just before it executes.
    virtual void BeforeSubmit(uint32_t frameSlot, uint32_t imageIndex) = 0;
    virtual SwapchainStatus Present(uint32_t imageIndex) = 0;
};

// The NVRHI device on one graphics API, what it can do, and the swapchains it makes. Vulkan's objects
// stay reachable for the code that is still Vulkan's alone (its acceleration structures, NGX's Vulkan
// entry points); on Direct3D 12 those getters return null.
class GpuDevice
{
  public:
    virtual ~GpuDevice() = default;

    // What the renderer records and creates resources with: NVRHI's validation layer when it is on.
    virtual nvrhi::IDevice* Get() const = 0;
    nvrhi::GraphicsAPI GetApi() const
    {
        return Get()->getGraphicsAPI();
    }
    bool IsVulkan() const
    {
        return GetApi() == nvrhi::GraphicsAPI::VULKAN;
    }
    virtual std::string GetAdapterName() const = 0;

    // Hardware ray tracing: acceleration structures and ray queries in compute and fragment shaders.
    virtual bool SupportsRayQuery() const = 0;
    virtual bool SupportsBlockCompression() const = 0;
    virtual bool SupportsIndependentBlend() const = 0;
    // A bindless table's unused slots may be written while frames read the others.
    virtual bool SupportsUpdateUnusedWhilePending() const = 0;
    // Video memory the CPU writes directly (resizable BAR; D3D12's GPU upload heap).
    virtual bool HasLargeHostVisibleDeviceMemory() const = 0;
    virtual float GetMaxSamplerAnisotropy() const = 0;
    virtual GpuLocalMemory QueryLocalMemory() const = 0;

    // The size a swapchain made for window now would have.
    virtual VkExtent2D GetSwapchainExtent(SDL_Window* window) const = 0;
    virtual std::unique_ptr<GpuSwapchain> CreateSwapchain(SDL_Window* window, bool preferHdr) = 0;

    virtual VulkanInstance* GetVulkanInstance() const
    {
        return nullptr;
    }
    virtual VulkanDevice* GetVulkanDevice() const
    {
        return nullptr;
    }
    virtual nvrhi::vulkan::IDevice* GetNvrhiVulkan() const
    {
        return nullptr;
    }
    // NVRHI's D3D12 device under the validation layer (its native acceleration structure handles).
    virtual nvrhi::d3d12::IDevice* GetNvrhiD3D12() const
    {
        return nullptr;
    }
};

// The Vulkan device (VulkanInstance, VulkanDevice and NVRHI over them). dlssExtensions: whether the
// instance and device extensions NGX needs are asked for (VulkanDlss).
std::unique_ptr<GpuDevice> CreateVulkanGpuDevice(SDL_Window* window, bool allowRayQuery);
#if MINIENGINE_WITH_D3D12
// The Direct3D 12 device on the high-performance adapter, with the Agility SDK's runtime.
std::unique_ptr<GpuDevice> CreateD3D12GpuDevice(bool allowRayQuery);
#endif
}
