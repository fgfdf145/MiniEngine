#pragma once

#include "common.h"

namespace me
{

class VulkanInstance
{
  public:
    explicit VulkanInstance(SDL_Window* window);
    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    VkInstance GetHandle() const;
    VkSurfaceKHR GetSurface() const;

  private:
    std::vector<const char*> GetRequiredExtensions(bool enableValidation) const;
    bool IsValidationLayerAvailable() const;
    void CreateDebugMessenger();
    void DestroyDebugMessenger();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
};
}
