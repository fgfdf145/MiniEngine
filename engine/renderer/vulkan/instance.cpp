#include "instance.h"

#include <log/log.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>

namespace
{
// Vulkan validation is compiled in for Debug builds only: it catches sync/layout/usage errors
// during development, and routing its output through the engine log means the `--frames 60`
// smoke test ("exit code 0 and no error in the log") covers Vulkan misuse automatically.
#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

// The engine never sets VkSwapchainCreateInfoKHR::flags, but third-party implicit layers
// (OBS-style capture hooks sit between the application and the validation layer) inject
// VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR into our create info, which validation then
// attributes to us. Downgrade that specific complaint so the smoke test's "no error in the
// log" check doesn't fail on machines with capture software installed.
bool IsExternalLayerSwapchainFlagsArtifact(const VkDebugUtilsMessengerCallbackDataEXT* callbackData)
{
    return callbackData && callbackData->pMessageIdName && callbackData->pMessage &&
           std::strcmp(callbackData->pMessageIdName, "VUID-VkSwapchainCreateInfoKHR-flags-parameter") == 0 &&
           std::strstr(callbackData->pMessage, "VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR") != nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*
)
{
    const char* message = (callbackData && callbackData->pMessage) ? callbackData->pMessage : "<no message>";
    if (IsExternalLayerSwapchainFlagsArtifact(callbackData))
    {
        LOG_WARN("[vulkan] (injected by an external implicit layer, not engine code) {}", message);
    }
    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        LOG_ERROR("[vulkan] {}", message);
    }
    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
    {
        LOG_WARN("[vulkan] {}", message);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugMessengerCallback;
    return createInfo;
}
}

VulkanInstance::VulkanInstance(SDL_Window* window)
{
    if (!window)
    {
        throw std::runtime_error("SDL window is null while creating a Vulkan instance");
    }

    const bool enableValidation = kEnableValidationLayers && IsValidationLayerAvailable();
    if (kEnableValidationLayers && !enableValidation)
    {
        LOG_WARN("Vulkan validation layer '{}' not available; running without validation", kValidationLayerName);
    }

    const std::vector<const char*> extensions = GetRequiredExtensions(enableValidation);

    LOG_INFO("Creating Vulkan instance (validation: {})", enableValidation ? "on" : "off");
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
#if defined(__APPLE__)
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    // Chain a messenger create info into the instance itself so validation covers
    // vkCreateInstance/vkDestroyInstance, which the regular messenger cannot observe.
    VkDebugUtilsMessengerCreateInfoEXT instanceDebugInfo = MakeDebugMessengerCreateInfo();
    if (enableValidation)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &kValidationLayerName;
        createInfo.pNext = &instanceDebugInfo;
    }

    CheckVulkan(vkCreateInstance(&createInfo, nullptr, &m_instance), "Failed to create Vulkan instance");

    if (enableValidation)
    {
        CreateDebugMessenger();
    }

    if (!SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &m_surface))
    {
        DestroyDebugMessenger();
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
        SDL_Vulkan_DestroySurface(m_instance, m_surface, nullptr);
        LOG_INFO("Vulkan surface destroyed");
    }
    DestroyDebugMessenger();
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

std::vector<const char*> VulkanInstance::GetRequiredExtensions(bool enableValidation) const
{
    Uint32 extensionCount = 0;
    const char* const* extensionNames = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!extensionNames || extensionCount == 0)
    {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }

    std::vector<const char*> extensions(extensionNames, extensionNames + extensionCount);

#if defined(__APPLE__)
    // MoltenVK is a portability-conformant implementation; the loader only
    // enumerates it when portability enumeration is requested.
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    if (enableValidation)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

bool VulkanInstance::IsValidationLayerAvailable() const
{
    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS || layerCount == 0)
    {
        return false;
    }

    std::vector<VkLayerProperties> layers(layerCount);
    if (vkEnumerateInstanceLayerProperties(&layerCount, layers.data()) != VK_SUCCESS)
    {
        return false;
    }

    for (const VkLayerProperties& layer : layers)
    {
        if (std::strcmp(layer.layerName, kValidationLayerName) == 0)
        {
            return true;
        }
    }
    return false;
}

void VulkanInstance::CreateDebugMessenger()
{
    const auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT")
    );
    if (!createFn)
    {
        LOG_WARN("vkCreateDebugUtilsMessengerEXT unavailable; Vulkan messages will not reach the engine log");
        return;
    }

    const VkDebugUtilsMessengerCreateInfoEXT createInfo = MakeDebugMessengerCreateInfo();
    CheckVulkan(createFn(m_instance, &createInfo, nullptr, &m_debugMessenger), "Failed to create Vulkan debug messenger");
}

void VulkanInstance::DestroyDebugMessenger()
{
    if (m_debugMessenger == VK_NULL_HANDLE || m_instance == VK_NULL_HANDLE)
    {
        return;
    }

    const auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT")
    );
    if (destroyFn)
    {
        destroyFn(m_instance, m_debugMessenger, nullptr);
    }
    m_debugMessenger = VK_NULL_HANDLE;
}
