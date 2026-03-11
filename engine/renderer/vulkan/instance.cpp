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

    SDL_PropertiesID windowProperties = SDL_GetWindowProperties(window);
    if (windowProperties == 0)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        throw std::runtime_error(std::string("SDL_GetWindowProperties failed: ") + SDL_GetError());
    }

#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(windowProperties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    HINSTANCE instanceHandle = static_cast<HINSTANCE>(
        SDL_GetPointerProperty(windowProperties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, GetModuleHandle(nullptr))
    );
    if (!hwnd || !instanceHandle)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        throw std::runtime_error("Failed to retrieve Win32 handles from SDL window");
    }

    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = instanceHandle;
    surfaceCreateInfo.hwnd = hwnd;

    CheckVulkan(vkCreateWin32SurfaceKHR(m_instance, &surfaceCreateInfo, nullptr, &m_surface), "Failed to create Win32 surface");
#else
    vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
    throw std::runtime_error("Win32 surface creation is not implemented on this platform");
#endif

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
#ifdef _WIN32
    return {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
#else
    throw std::runtime_error("Required Vulkan instance extensions are not implemented on this platform");
#endif
}
