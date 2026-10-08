#pragma once

#include "common.h"

#include <span>
#include <string>
#include <vector>

namespace me
{

class VulkanInstance
{
  public:
    // optionalExtensions are enabled where the loader offers them (DLSS's, VulkanDlss); whether all
    // of them were is OptionalExtensionsEnabled.
    explicit VulkanInstance(SDL_Window* window, std::span<const std::string> optionalExtensions = {});
    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    VkInstance GetHandle() const;
    VkSurfaceKHR GetSurface() const;
    bool OptionalExtensionsEnabled() const;
    // The instance extensions it was created with (NVRHI is told them, nvrhi_device.h).
    const std::vector<std::string>& GetEnabledExtensions() const;

  private:
    std::vector<const char*> GetRequiredExtensions(bool enableValidation, std::span<const std::string> optionalExtensions);
    bool IsValidationLayerAvailable() const;
    void CreateDebugMessenger();
    void DestroyDebugMessenger();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    bool m_optionalExtensionsEnabled = true;
    std::vector<std::string> m_enabledExtensions;
};
}
