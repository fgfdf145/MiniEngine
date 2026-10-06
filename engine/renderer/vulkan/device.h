#pragma once

#include "common.h"

#include <functional>
#include <string>

namespace me
{

class VulkanDevice
{
  public:
    // Extensions some feature asks for once the physical device is chosen (DLSS's, VulkanDlss).
    using OptionalExtensions = std::function<std::vector<std::string>(VkPhysicalDevice)>;

    // optionalExtensions' extensions are enabled where the device offers them; whether all of them
    // were is OptionalExtensionsEnabled. allowRayQuery false leaves hardware ray tracing off whatever
    // the device offers.
    VulkanDevice(VkInstance instance, VkSurfaceKHR surface, const OptionalExtensions& optionalExtensions = {}, bool allowRayQuery = true);
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkDevice GetHandle() const;
    VkPhysicalDevice GetPhysicalDevice() const;
    const QueueFamilyIndices& GetQueueFamilies() const;
    VkQueue GetGraphicsQueue() const;
    VkQueue GetPresentQueue() const;
    SwapchainSupportDetails QuerySwapchainSupport() const;
    VkSurfaceCapabilitiesKHR QuerySurfaceCapabilities() const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    // Whether material textures can be uploaded as BC7 and BC5: the textureCompressionBC feature
    // is enabled and all three formats sample with linear filtering. Decided once, at creation.
    bool SupportsBlockCompression() const;
    // Whether the independentBlend feature is enabled, which deferred decals need; without it they
    // stay forward shaded Blend items.
    bool SupportsIndependentBlend() const;
    bool OptionalExtensionsEnabled() const;
    // Whether hardware ray tracing is on: VK_KHR_acceleration_structure and VK_KHR_ray_query with
    // bufferDeviceAddress, enabled when the device offers all of them on Vulkan 1.2 or later. Without
    // it every ray walks the ray scene's own hierarchies in compute (ray_tracing_common.glsl).
    bool SupportsRayQuery() const;

  private:
    bool IsSuitable(VkPhysicalDevice device) const;
    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
    SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device) const;
    bool HasRequiredExtensions(VkPhysicalDevice device) const;

    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilies;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    bool m_supportsBlockCompression = false;
    bool m_supportsIndependentBlend = false;
    bool m_optionalExtensionsEnabled = true;
    bool m_supportsRayQuery = false;
};
}
