#include "gpu_device.h"

#include "command.h"
#include "device.h"
#include "dlss.h"
#include "instance.h"
#include "nvrhi_device.h"
#include "nvrhi_native.h"
#include "nvrhi_pass.h"
#include "swapchain.h"

#include <engine/core/log/log.h>

#include <nvrhi/vulkan.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
VkSemaphore CreateBinarySemaphore(VkDevice device, const char* what)
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    CheckVulkan(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), what);
    return semaphore;
}

SwapchainStatus ToStatus(VkResult result, const char* what)
{
    switch (result)
    {
    case VK_SUCCESS:
        return SwapchainStatus::Ok;
    case VK_SUBOPTIMAL_KHR:
        return SwapchainStatus::Suboptimal;
    case VK_ERROR_OUT_OF_DATE_KHR:
        return SwapchainStatus::OutOfDate;
    default:
        CheckVulkan(result, what);
        return SwapchainStatus::OutOfDate;
    }
}

class VulkanGpuSwapchain final : public GpuSwapchain
{
  public:
    VulkanGpuSwapchain(SDL_Window* window, VulkanInstance& instance, VulkanDevice& device, NvrhiDevice& nvrhi, bool preferHdr)
        : m_device(device), m_nvrhi(nvrhi)
    {
        m_swapchain = std::make_unique<VulkanSwapchain>(
            window, device.GetHandle(), instance.GetSurface(), device.GetQueueFamilies(), device.QuerySwapchainSupport(), preferHdr);
        for (const VkImage image : m_swapchain->GetImages())
        {
            nvrhi::TextureDesc desc;
            desc.width = m_swapchain->GetExtent().width;
            desc.height = m_swapchain->GetExtent().height;
            desc.format = ToNvrhiFormat(m_swapchain->GetImageFormat());
            desc.dimension = nvrhi::TextureDimension::Texture2D;
            desc.isRenderTarget = true;
            desc.debugName = "Swapchain image";
            nvrhi::TextureHandle texture = m_nvrhi.Get()->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), desc);
            if (!texture)
            {
                throw std::runtime_error("Failed to wrap a swapchain image for NVRHI");
            }
            m_images.push_back(std::move(texture));
            m_renderFinished.push_back(CreateBinarySemaphore(device.GetHandle(), "Failed to create a render finished semaphore"));
        }
        for (uint32_t slot = 0; slot < VulkanCommandContext::kMaxFramesInFlight; ++slot)
        {
            m_imageAvailable.push_back(CreateBinarySemaphore(device.GetHandle(), "Failed to create an image available semaphore"));
        }
    }

    ~VulkanGpuSwapchain() override
    {
        // The caller has waited for every frame: nothing waits on the semaphores any more.
        m_images.clear();
        for (VkSemaphore semaphore : m_imageAvailable)
        {
            vkDestroySemaphore(m_device.GetHandle(), semaphore, nullptr);
        }
        for (VkSemaphore semaphore : m_renderFinished)
        {
            vkDestroySemaphore(m_device.GetHandle(), semaphore, nullptr);
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
        return m_swapchain->GetImageFormat();
    }
    VkExtent2D GetExtent() const override
    {
        return m_swapchain->GetExtent();
    }
    bool IsHdr() const override
    {
        return m_swapchain->IsHdr();
    }

    SwapchainStatus Acquire(uint32_t frameSlot, uint32_t& imageIndex) override
    {
        const VkResult result = vkAcquireNextImageKHR(
            m_device.GetHandle(), m_swapchain->GetHandle(), UINT64_MAX, m_imageAvailable.at(frameSlot), VK_NULL_HANDLE, &imageIndex);
        return ToStatus(result, "Failed to acquire a swapchain image");
    }

    void BeforeSubmit(uint32_t frameSlot, uint32_t imageIndex) override
    {
        // The frame's command list waits for the image and signals the present's wait.
        nvrhi::vulkan::IDevice* vulkan = m_nvrhi.GetVulkan();
        vulkan->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, m_imageAvailable.at(frameSlot), 0);
        vulkan->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, m_renderFinished.at(imageIndex), 0);
    }

    SwapchainStatus Present(uint32_t imageIndex) override
    {
        const VkSemaphore waitSemaphores[] = {m_renderFinished.at(imageIndex)};
        const VkSwapchainKHR swapchain = m_swapchain->GetHandle();
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = waitSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;
        return ToStatus(vkQueuePresentKHR(m_device.GetPresentQueue(), &presentInfo), "Failed to present a swapchain image");
    }

  private:
    VulkanDevice& m_device;
    NvrhiDevice& m_nvrhi;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::vector<nvrhi::TextureHandle> m_images;
    // Per frame slot: the semaphore the acquire signals. Per image: the one the present waits on
    // (the presentation engine may still wait on it after the frame has finished, so it is reused
    // only when its image comes back).
    std::vector<VkSemaphore> m_imageAvailable;
    std::vector<VkSemaphore> m_renderFinished;
};

class VulkanGpuDevice final : public GpuDevice
{
  public:
    VulkanGpuDevice(SDL_Window* window, bool allowRayQuery)
    {
        uint32_t apiVersion = 0;
        CheckVulkan(vkEnumerateInstanceVersion(&apiVersion), "Failed to query Vulkan runtime version");
        LOG_INFO(
            "Vulkan runtime API version: {}.{}.{}",
            VK_API_VERSION_MAJOR(apiVersion),
            VK_API_VERSION_MINOR(apiVersion),
            VK_API_VERSION_PATCH(apiVersion));

        // DLSS's extensions are asked for as the instance and the device are made; without all of them
        // DLSS stays off and nothing else changes.
        m_instance = std::make_unique<VulkanInstance>(window, VulkanDlss::RequiredInstanceExtensions());
        const VkInstance instance = m_instance->GetHandle();
        m_device = std::make_unique<VulkanDevice>(
            instance,
            m_instance->GetSurface(),
            [instance](VkPhysicalDevice physicalDevice)
            {
                return VulkanDlss::RequiredDeviceExtensions(instance, physicalDevice);
            },
            allowRayQuery);
        m_nvrhi = std::make_unique<NvrhiDevice>(*m_instance, *m_device);

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(m_device->GetPhysicalDevice(), &features);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_device->GetPhysicalDevice(), &properties);
        m_maxAnisotropy = features.samplerAnisotropy ? std::min(16.0f, properties.limits.maxSamplerAnisotropy) : 0.0f;
        m_adapterName = properties.deviceName;
        SetNativeViewportConvention(nvrhi::GraphicsAPI::VULKAN);
    }

    ~VulkanGpuDevice() override
    {
        m_nvrhi.reset();
        m_device.reset();
        m_instance.reset();
    }

    nvrhi::IDevice* Get() const override
    {
        return m_nvrhi->Get();
    }
    std::string GetAdapterName() const override
    {
        return m_adapterName;
    }
    bool SupportsRayQuery() const override
    {
        return m_device->SupportsRayQuery();
    }
    bool SupportsBlockCompression() const override
    {
        return m_device->SupportsBlockCompression();
    }
    bool SupportsIndependentBlend() const override
    {
        return m_device->SupportsIndependentBlend();
    }
    bool SupportsUpdateUnusedWhilePending() const override
    {
        return m_device->SupportsUpdateUnusedWhilePending();
    }
    bool HasLargeHostVisibleDeviceMemory() const override
    {
        return m_device->HasLargeHostVisibleDeviceMemory();
    }
    float GetMaxSamplerAnisotropy() const override
    {
        return m_maxAnisotropy;
    }
    GpuLocalMemory QueryLocalMemory() const override
    {
        const VulkanDevice::LocalMemory local = m_device->QueryLocalMemory();
        return GpuLocalMemory{local.usage, local.budget, local.measured};
    }
    VkExtent2D GetSwapchainExtent(SDL_Window* window) const override
    {
        return VulkanSwapchain::ChooseExtent(window, m_device->QuerySurfaceCapabilities());
    }
    std::unique_ptr<GpuSwapchain> CreateSwapchain(SDL_Window* window, bool preferHdr) override
    {
        return std::make_unique<VulkanGpuSwapchain>(window, *m_instance, *m_device, *m_nvrhi, preferHdr);
    }
    VulkanInstance* GetVulkanInstance() const override
    {
        return m_instance.get();
    }
    VulkanDevice* GetVulkanDevice() const override
    {
        return m_device.get();
    }
    nvrhi::vulkan::IDevice* GetNvrhiVulkan() const override
    {
        return m_nvrhi->GetVulkan();
    }

  private:
    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::unique_ptr<NvrhiDevice> m_nvrhi;
    float m_maxAnisotropy = 0.0f;
    std::string m_adapterName;
};
}

std::unique_ptr<GpuDevice> CreateVulkanGpuDevice(SDL_Window* window, bool allowRayQuery)
{
    return std::make_unique<VulkanGpuDevice>(window, allowRayQuery);
}
}
