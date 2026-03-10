#include "swapchain.h"

#include <log/log.h>

#include <algorithm>

VulkanSwapchain::VulkanSwapchain(
    SDL_Window* window,
    VkDevice device,
    VkSurfaceKHR surface,
    const QueueFamilyIndices& queueFamilies,
    const SwapchainSupportDetails& supportDetails
)
    : m_device(device)
{
    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(supportDetails.formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(supportDetails.presentModes);
    const VkExtent2D extent = ChooseExtent(window, supportDetails.capabilities);

    uint32_t imageCount = supportDetails.capabilities.minImageCount + 1;
    if (supportDetails.capabilities.maxImageCount > 0 && imageCount > supportDetails.capabilities.maxImageCount)
    {
        imageCount = supportDetails.capabilities.maxImageCount;
    }

    const uint32_t queueFamilyIndices[] = {
        queueFamilies.graphicsFamily.value(),
        queueFamilies.presentFamily.value()
    };

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (queueFamilies.graphicsFamily != queueFamilies.presentFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = supportDetails.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    CheckVulkan(vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain), "Failed to create swapchain");

    CheckVulkan(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, nullptr), "Failed to get swapchain image count");
    m_images.resize(imageCount);
    CheckVulkan(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, m_images.data()), "Failed to get swapchain images");

    m_imageFormat = surfaceFormat.format;
    m_extent = extent;
    CreateImageViews();

    LOG_INFO("Swapchain created successfully with {} images", imageCount);
}

VulkanSwapchain::~VulkanSwapchain()
{
    for (VkImageView imageView : m_imageViews)
    {
        vkDestroyImageView(m_device, imageView, nullptr);
    }

    if (m_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        LOG_INFO("Swapchain destroyed");
    }
}

VkSwapchainKHR VulkanSwapchain::GetHandle() const
{
    return m_swapchain;
}

VkFormat VulkanSwapchain::GetImageFormat() const
{
    return m_imageFormat;
}

VkExtent2D VulkanSwapchain::GetExtent() const
{
    return m_extent;
}

const std::vector<VkImageView>& VulkanSwapchain::GetImageViews() const
{
    return m_imageViews;
}

VkSurfaceFormatKHR VulkanSwapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
    for (const auto& availableFormat : formats)
    {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return availableFormat;
        }
    }

    return formats.front();
}

VkPresentModeKHR VulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
    for (VkPresentModeKHR presentMode : presentModes)
    {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return presentMode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::ChooseExtent(SDL_Window* window, const VkSurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &width, &height))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    VkExtent2D actualExtent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };

    actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return actualExtent;
}

void VulkanSwapchain::CreateImageViews()
{
    m_imageViews.resize(m_images.size());

    for (size_t i = 0; i < m_images.size(); ++i)
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = m_images[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = m_imageFormat;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        CheckVulkan(vkCreateImageView(m_device, &createInfo, nullptr, &m_imageViews[i]), "Failed to create image view");
    }
}
