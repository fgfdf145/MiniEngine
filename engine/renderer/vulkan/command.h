#pragma once

#include "common.h"

class VulkanCommandContext
{
public:
    VulkanCommandContext(
        VkDevice device,
        const QueueFamilyIndices& queueFamilies,
        VkRenderPass renderPass,
        VkPipeline graphicsPipeline,
        VkExtent2D extent,
        VkBuffer vertexBuffer,
        uint32_t vertexCount,
        const std::vector<VkFramebuffer>& framebuffers
    );
    ~VulkanCommandContext();

    VulkanCommandContext(const VulkanCommandContext&) = delete;
    VulkanCommandContext& operator=(const VulkanCommandContext&) = delete;

    VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex);
    void Submit(VkQueue graphicsQueue, uint32_t imageIndex);
    VkResult Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex);

private:
    void CreateCommandPool(const QueueFamilyIndices& queueFamilies);
    void AllocateCommandBuffers(size_t commandBufferCount);
    void RecordCommandBuffers(
        VkRenderPass renderPass,
        VkPipeline graphicsPipeline,
        VkExtent2D extent,
        VkBuffer vertexBuffer,
        uint32_t vertexCount,
        const std::vector<VkFramebuffer>& framebuffers
    );
    void CreateSyncObjects();

    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    VkSemaphore m_imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence m_inFlightFence = VK_NULL_HANDLE;
};
