#pragma once

#include "common.h"

class VulkanSwapchain
{
public:
    VulkanSwapchain(
        SDL_Window* window,
        VkDevice device,
        VkSurfaceKHR surface,
        const QueueFamilyIndices& queueFamilies,
        const SwapchainSupportDetails& supportDetails
    );
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    VkSwapchainKHR GetHandle() const;
    VkFormat GetImageFormat() const;
    VkExtent2D GetExtent() const;
    const std::vector<VkImageView>& GetImageViews() const;

private:
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    VkExtent2D ChooseExtent(SDL_Window* window, const VkSurfaceCapabilitiesKHR& capabilities) const;
    void CreateImageViews();

    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
};
