#pragma once

#include "common.h"

namespace me
{

class VulkanSwapchain
{
  public:
    VulkanSwapchain(
        SDL_Window* window,
        VkDevice device,
        VkSurfaceKHR surface,
        const QueueFamilyIndices& queueFamilies,
        const SwapchainSupportDetails& supportDetails,
        bool preferHdr);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    VkSwapchainKHR GetHandle() const;
    VkFormat GetImageFormat() const;
    // True when the swapchain is HDR10: 10-bit, PQ-encoded Rec.2020 (see hdr_output.glsl).
    bool IsHdr() const;
    VkExtent2D GetExtent() const;
    const std::vector<VkImageView>& GetImageViews() const;

    // The extent a swapchain created now would get: the surface's current extent, or the window's
    // pixel size clamped to the surface limits when the surface leaves it to the swapchain.
    static VkExtent2D ChooseExtent(SDL_Window* window, const VkSurfaceCapabilitiesKHR& capabilities);

  private:
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats, bool preferHdr) const;
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    void CreateImageViews();

    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
};
}
