#include "device.h"

#include <log/log.h>

#include <set>

namespace
{
const std::vector<const char*> kRequiredExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};
}

VulkanDevice::VulkanDevice(VkInstance instance, VkSurfaceKHR surface)
    : m_surface(surface)
{
    uint32_t deviceCount = 0;
    CheckVulkan(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr), "Failed to enumerate physical devices");
    if (deviceCount == 0)
    {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    CheckVulkan(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()), "Failed to enumerate physical devices");

    for (VkPhysicalDevice device : devices)
    {
        if (IsSuitable(device))
        {
            m_physicalDevice = device;
            m_queueFamilies = FindQueueFamilies(device);

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            LOG_INFO("Selected physical device: {}", properties.deviceName);
            break;
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE)
    {
        throw std::runtime_error("Failed to find a suitable Vulkan physical device");
    }

    std::set<uint32_t> uniqueQueueFamilies = {
        m_queueFamilies.graphicsFamily.value(),
        m_queueFamilies.presentFamily.value()
    };

    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(kRequiredExtensions.size());
    createInfo.ppEnabledExtensionNames = kRequiredExtensions.data();

    CheckVulkan(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device), "Failed to create logical device");
    vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily.value(), 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.presentFamily.value(), 0, &m_presentQueue);
    LOG_INFO("Logical device created successfully");
}

VulkanDevice::~VulkanDevice()
{
    if (m_device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_device, nullptr);
        LOG_INFO("Logical device destroyed");
    }
}

VkDevice VulkanDevice::GetHandle() const
{
    return m_device;
}

VkPhysicalDevice VulkanDevice::GetPhysicalDevice() const
{
    return m_physicalDevice;
}

const QueueFamilyIndices& VulkanDevice::GetQueueFamilies() const
{
    return m_queueFamilies;
}

VkQueue VulkanDevice::GetGraphicsQueue() const
{
    return m_graphicsQueue;
}

VkQueue VulkanDevice::GetPresentQueue() const
{
    return m_presentQueue;
}

SwapchainSupportDetails VulkanDevice::QuerySwapchainSupport() const
{
    return QuerySwapchainSupport(m_physicalDevice);
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        const bool typeMatches = (typeFilter & (1u << i)) != 0;
        const bool propertiesMatch = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeMatches && propertiesMatch)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable device memory type");
}

bool VulkanDevice::IsSuitable(VkPhysicalDevice device) const
{
    const QueueFamilyIndices indices = FindQueueFamilies(device);
    const bool hasExtensions = HasRequiredExtensions(device);
    const SwapchainSupportDetails support = hasExtensions ? QuerySwapchainSupport(device) : SwapchainSupportDetails{};
    return indices.IsComplete() && hasExtensions && !support.formats.empty() && !support.presentModes.empty();
}

QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) const
{
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        CheckVulkan(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport), "Failed to query present support");
        if (presentSupport == VK_TRUE)
        {
            indices.presentFamily = i;
        }

        if (indices.IsComplete())
        {
            break;
        }
    }

    return indices;
}

SwapchainSupportDetails VulkanDevice::QuerySwapchainSupport(VkPhysicalDevice device) const
{
    SwapchainSupportDetails details;

    CheckVulkan(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities), "Failed to get surface capabilities");

    uint32_t formatCount = 0;
    CheckVulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr), "Failed to get surface formats");
    if (formatCount > 0)
    {
        details.formats.resize(formatCount);
        CheckVulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, details.formats.data()), "Failed to get surface formats");
    }

    uint32_t presentModeCount = 0;
    CheckVulkan(vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr), "Failed to get present modes");
    if (presentModeCount > 0)
    {
        details.presentModes.resize(presentModeCount);
        CheckVulkan(vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, details.presentModes.data()), "Failed to get present modes");
    }

    return details;
}

bool VulkanDevice::HasRequiredExtensions(VkPhysicalDevice device) const
{
    uint32_t extensionCount = 0;
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "Failed to enumerate device extensions");

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()), "Failed to enumerate device extensions");

    std::set<std::string> requiredExtensions(kRequiredExtensions.begin(), kRequiredExtensions.end());
    for (const auto& extension : availableExtensions)
    {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}
