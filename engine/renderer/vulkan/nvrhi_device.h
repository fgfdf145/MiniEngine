#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

namespace me
{

class VulkanInstance;
class VulkanDevice;

// NVRHI over the engine's own Vulkan instance and device (docs/design/2026-10-08-nvrhi-backend-design.md):
// VulkanInstance and VulkanDevice choose the device, its features and extensions (DLSS's among them)
// and the swapchain; NVRHI records and submits on that device. Its messages go to the engine log.
// MINIENGINE_NVRHI_VALIDATION=1 puts NVRHI's validation layer in front of it.
class NvrhiDevice
{
  public:
    NvrhiDevice(const VulkanInstance& instance, const VulkanDevice& device);
    ~NvrhiDevice();

    NvrhiDevice(const NvrhiDevice&) = delete;
    NvrhiDevice& operator=(const NvrhiDevice&) = delete;

    // What the renderer records and creates resources with: the validation layer when it is on.
    nvrhi::IDevice* Get() const
    {
        return m_device.Get();
    }
    // The Vulkan backend itself, for its semaphore calls (acquire and present).
    nvrhi::vulkan::IDevice* GetVulkan() const
    {
        return m_vulkanDevice.Get();
    }

  private:
    class MessageLog : public nvrhi::IMessageCallback
    {
      public:
        void message(nvrhi::MessageSeverity severity, const char* messageText) override;
    };

    MessageLog m_messageLog;
    nvrhi::vulkan::DeviceHandle m_vulkanDevice;
    nvrhi::DeviceHandle m_device;
};
}
