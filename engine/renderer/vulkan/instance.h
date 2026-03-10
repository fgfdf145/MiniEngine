#pragma once

#include "common.h"

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
    std::vector<const char*> GetRequiredExtensions() const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
};
