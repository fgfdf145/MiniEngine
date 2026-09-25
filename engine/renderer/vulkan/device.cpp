#include "device.h"

#include <engine/core/log/log.h>

#include <set>

namespace me
{

namespace
{
const std::vector<const char*> kRequiredExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};

// Defined in vulkan_beta.h; spelled out here so no VK_ENABLE_BETA_EXTENSIONS is needed.
// The spec requires enabling this extension whenever the device advertises it (MoltenVK does).
constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";

std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice device)
{
    uint32_t extensionCount = 0;
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "Failed to enumerate device extensions");

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()), "Failed to enumerate device extensions");

    return availableExtensions;
}
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
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            // The geometry pass writes eight G-buffer targets at once, and the shadow passes push 144
            // bytes of constants (Vulkan guarantees 128).
            if (properties.limits.maxPushConstantsSize < 144)
            {
                LOG_WARN(
                    "Skipping {}: it offers {} bytes of push constants and the shadow passes need 144",
                    properties.deviceName,
                    properties.limits.maxPushConstantsSize);
                continue;
            }
            if (properties.limits.maxColorAttachments < 8)
            {
                LOG_WARN(
                    "Skipping {}: it offers {} colour attachments and the G-buffer needs 8",
                    properties.deviceName,
                    properties.limits.maxColorAttachments);
                continue;
            }
            m_physicalDevice = device;
            m_queueFamilies = FindQueueFamilies(device);
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
        m_queueFamilies.presentFamily.value()};

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

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = supportedFeatures.samplerAnisotropy;

    // BC textures need the feature and, for each format material textures use, sampling with
    // linear filtering. Anything less and every texture stays RGBA8.
    m_supportsBlockCompression = supportedFeatures.textureCompressionBC == VK_TRUE;
    for (const VkFormat format : {VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK})
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
        constexpr VkFormatFeatureFlags kRequired =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        m_supportsBlockCompression =
            m_supportsBlockCompression && (properties.optimalTilingFeatures & kRequired) == kRequired;
    }
    deviceFeatures.textureCompressionBC = m_supportsBlockCompression ? VK_TRUE : VK_FALSE;
    LOG_INFO(
        "Block-compressed textures: {}",
        m_supportsBlockCompression ? "BC7/BC5" : "unsupported, textures stay RGBA8");

    std::vector<const char*> enabledExtensions = kRequiredExtensions;
    for (const auto& extension : EnumerateDeviceExtensions(m_physicalDevice))
    {
        if (std::string(extension.extensionName) == kPortabilitySubsetExtensionName)
        {
            enabledExtensions.push_back(kPortabilitySubsetExtensionName);
            LOG_INFO("Enabling device extension: {}", kPortabilitySubsetExtensionName);
            break;
        }
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

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

bool VulkanDevice::SupportsBlockCompression() const
{
    return m_supportsBlockCompression;
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

VkSurfaceCapabilitiesKHR VulkanDevice::QuerySurfaceCapabilities() const
{
    VkSurfaceCapabilitiesKHR capabilities{};
    CheckVulkan(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities),
        "Failed to get surface capabilities");
    return capabilities;
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
        // The frame records compute work (the exposure histogram) into the same command buffer as
        // its graphics passes, so the family has to support both. Vulkan guarantees a device with
        // graphics has such a family.
        constexpr VkQueueFlags kRequiredFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((queueFamilies[i].queueFlags & kRequiredFlags) == kRequiredFlags)
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
    std::set<std::string> requiredExtensions(kRequiredExtensions.begin(), kRequiredExtensions.end());
    for (const auto& extension : EnumerateDeviceExtensions(device))
    {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}
}
