#include "instance.h"

#include <log/log.h>

VulkanInstance::VulkanInstance(SDL_Window* window)
{
    const std::vector<const char*> extensions = GetRequiredExtensions();

    LOG_INFO("Creating Vulkan instance");
    for (size_t i = 0; i < extensions.size(); ++i)
    {
        LOG_INFO("Vulkan extension[{}]: {}", i, extensions[i]);
    }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "MiniEngine";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "MiniEngine";
    applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    CheckVulkan(vkCreateInstance(&createInfo, nullptr, &m_instance), "Failed to create Vulkan instance");
    if (!SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &m_surface))
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }

    LOG_INFO("Vulkan instance created successfully");
}

VulkanInstance::~VulkanInstance()
{
    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        LOG_INFO("Vulkan surface destroyed");
    }
    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        LOG_INFO("Vulkan instance destroyed");
    }
}

VkInstance VulkanInstance::GetHandle() const
{
    return m_instance;
}

VkSurfaceKHR VulkanInstance::GetSurface() const
{
    return m_surface;
}

std::vector<const char*> VulkanInstance::GetRequiredExtensions() const
{
    Uint32 extensionCount = 0;
    const char* const* extensionNames = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!extensionNames)
    {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }

    return std::vector<const char*>(extensionNames, extensionNames + extensionCount);
}
