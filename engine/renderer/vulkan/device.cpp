#include "device.h"
#include "memory_pool.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <cstdint>
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

// Hardware ray tracing, all or none (SupportsRayQuery).
const std::vector<const char*> kRayQueryExtensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice device)
{
    uint32_t extensionCount = 0;
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "Failed to enumerate device extensions");

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    CheckVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()), "Failed to enumerate device extensions");

    return availableExtensions;
}
}

VulkanDevice::VulkanDevice(VkInstance instance, VkSurfaceKHR surface, const OptionalExtensions& optionalExtensions, bool allowRayQuery)
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
    // Deferred decals blend some G-buffer channels and mask the rest, one blend state per attachment.
    m_supportsIndependentBlend = supportedFeatures.independentBlend == VK_TRUE;
    deviceFeatures.independentBlend = supportedFeatures.independentBlend;

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

    const std::vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(m_physicalDevice);
    const auto isAvailable = [&availableExtensions](const std::string& name)
    {
        return std::any_of(availableExtensions.begin(), availableExtensions.end(), [&name](const VkExtensionProperties& extension)
                           {
                               return name == extension.extensionName;
                           });
    };
    std::vector<const char*> enabledExtensions = kRequiredExtensions;
    if (isAvailable(kPortabilitySubsetExtensionName))
    {
        enabledExtensions.push_back(kPortabilitySubsetExtensionName);
        LOG_INFO("Enabling device extension: {}", kPortabilitySubsetExtensionName);
    }
    // The driver's per-process budget and usage of each heap, which the world's streaming fits in.
    m_supportsMemoryBudget = isAvailable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (m_supportsMemoryBudget)
    {
        enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        LOG_INFO("Enabling device extension: {}", VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

    // The optional ones, and the buffer device address feature when one of them is that extension
    // (core since Vulkan 1.2, where the feature still has to be asked for).
    const std::vector<std::string> optional = optionalExtensions ? optionalExtensions(m_physicalDevice) : std::vector<std::string>{};
    bool wantsBufferDeviceAddress = false;
    for (const std::string& name : optional)
    {
        if (std::any_of(enabledExtensions.begin(), enabledExtensions.end(), [&name](const char* enabled)
                        {
                            return name == enabled;
                        }))
        {
            continue;
        }
        if (!isAvailable(name))
        {
            LOG_WARN("Optional device extension {} is not available", name);
            m_optionalExtensionsEnabled = false;
            continue;
        }
        enabledExtensions.push_back(name.c_str());
        LOG_INFO("Enabling device extension: {}", name);
        wantsBufferDeviceAddress = wantsBufferDeviceAddress || name.find("buffer_device_address") != std::string::npos;
    }

    // The Vulkan 1.2 features and hardware ray tracing's, queried through one chain.
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProperties);
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{};
    accelerationFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    const bool rayQueryExtensions =
        allowRayQuery && deviceProperties.apiVersion >= VK_API_VERSION_1_2 &&
        std::all_of(kRayQueryExtensions.begin(), kRayQueryExtensions.end(), isAvailable);
    if (rayQueryExtensions)
    {
        accelerationFeatures.pNext = &rayQueryFeatures;
        vulkan12Features.pNext = &accelerationFeatures;
    }
    // Below Vulkan 1.2 there is no 1.2 block, and neither buffer device address nor ray tracing.
    if (deviceProperties.apiVersion >= VK_API_VERSION_1_2)
    {
        VkPhysicalDeviceFeatures2 query{};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &vulkan12Features;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &query);
    }
    if (wantsBufferDeviceAddress)
    {
        m_optionalExtensionsEnabled = m_optionalExtensionsEnabled && vulkan12Features.bufferDeviceAddress == VK_TRUE;
    }
    // Hit shading samples every material's textures through one array indexed per hit (the ray scene's
    // texture table), which needs descriptor indexing; every GPU with ray queries has it.
    const bool descriptorIndexing = vulkan12Features.runtimeDescriptorArray == VK_TRUE &&
                                    vulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
                                    vulkan12Features.descriptorBindingPartiallyBound == VK_TRUE &&
                                    vulkan12Features.descriptorBindingVariableDescriptorCount == VK_TRUE;
    // Hit shading reads the meshes' vertex and index buffers through 64-bit pointers
    // (ray_hit_common.slang), so it needs 64-bit integers in shaders as well.
    m_supportsRayQuery = rayQueryExtensions && vulkan12Features.bufferDeviceAddress == VK_TRUE && descriptorIndexing &&
                         supportedFeatures.shaderInt64 == VK_TRUE &&
                         accelerationFeatures.accelerationStructure == VK_TRUE && rayQueryFeatures.rayQuery == VK_TRUE;
    LOG_INFO(
        "Hardware ray tracing: {}",
        m_supportsRayQuery ? "ray queries"
        : allowRayQuery    ? "unsupported, rays walk the compute hierarchies"
                           : "off (--no-ray-query), rays walk the compute hierarchies");

    // Only the features the engine uses go into the chain it enables. One Vulkan 1.2 block carries
    // buffer device address for both (it may not be chained beside the standalone feature struct).
    VkPhysicalDeviceRayQueryFeaturesKHR enabledRayQuery{};
    enabledRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    enabledRayQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAcceleration{};
    enabledAcceleration.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    enabledAcceleration.accelerationStructure = VK_TRUE;
    enabledAcceleration.pNext = &enabledRayQuery;
    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.bufferDeviceAddress = (wantsBufferDeviceAddress || m_supportsRayQuery) ? vulkan12Features.bufferDeviceAddress : VK_FALSE;
    if (m_supportsRayQuery)
    {
        enabled12.runtimeDescriptorArray = VK_TRUE;
        enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabled12.descriptorBindingPartiallyBound = VK_TRUE;
        enabled12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        m_supportsUpdateUnusedWhilePending = vulkan12Features.descriptorBindingUpdateUnusedWhilePending == VK_TRUE;
        enabled12.descriptorBindingUpdateUnusedWhilePending = m_supportsUpdateUnusedWhilePending ? VK_TRUE : VK_FALSE;
    }
    deviceFeatures.shaderInt64 = m_supportsRayQuery ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 enabledFeatures{};
    enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures.features = deviceFeatures;
    if (m_supportsRayQuery)
    {
        enabledExtensions.insert(enabledExtensions.end(), kRayQueryExtensions.begin(), kRayQueryExtensions.end());
        enabled12.pNext = &enabledAcceleration;
    }
    if (wantsBufferDeviceAddress || m_supportsRayQuery)
    {
        enabledFeatures.pNext = &enabled12;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &enabledFeatures;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
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
        // The memory pool's spare blocks (everything else is freed by then).
        VulkanMemoryPool::ReleaseEmptyBlocks(m_device, 0, SIZE_MAX);
        vkDestroyDevice(m_device, nullptr);
        LOG_INFO("Logical device destroyed");
    }
}

VulkanDevice::LocalMemory VulkanDevice::QueryLocalMemory() const
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = m_supportsMemoryBudget ? &budget : nullptr;
    vkGetPhysicalDeviceMemoryProperties2(m_physicalDevice, &properties);

    LocalMemory result;
    result.measured = m_supportsMemoryBudget;
    const VkPhysicalDeviceMemoryProperties& memory = properties.memoryProperties;
    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap)
    {
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
        {
            continue;
        }
        if (m_supportsMemoryBudget)
        {
            result.usage += budget.heapUsage[heap];
            result.budget += budget.heapBudget[heap];
        }
        else
        {
            result.budget += memory.memoryHeaps[heap].size / 5 * 4;
        }
    }
    return result;
}

bool VulkanDevice::OptionalExtensionsEnabled() const
{
    return m_optionalExtensionsEnabled;
}

VkDevice VulkanDevice::GetHandle() const
{
    return m_device;
}

bool VulkanDevice::SupportsBlockCompression() const
{
    return m_supportsBlockCompression;
}

bool VulkanDevice::SupportsIndependentBlend() const
{
    return m_supportsIndependentBlend;
}

bool VulkanDevice::SupportsRayQuery() const
{
    return m_supportsRayQuery;
}

bool VulkanDevice::SupportsUpdateUnusedWhilePending() const
{
    return m_supportsUpdateUnusedWhilePending;
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
