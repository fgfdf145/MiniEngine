#pragma once

#include "common.h"

#include <functional>

class VulkanCommandContext
{
public:
    VulkanCommandContext(VkDevice device, const QueueFamilyIndices& queueFamilies, size_t commandBufferCount);
    ~VulkanCommandContext();

    VulkanCommandContext(const VulkanCommandContext&) = delete;
    VulkanCommandContext& operator=(const VulkanCommandContext&) = delete;

    VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex);
    void RecordCommandBuffer(
        uint32_t imageIndex,
        VkRenderPass renderPass,
        VkFramebuffer framebuffer,
        VkExtent2D extent,
        VkPipeline graphicsPipeline,
        VkPipelineLayout pipelineLayout,
        VkBuffer vertexBuffer,
        VkBuffer indexBuffer,
        uint32_t indexCount,
        VkDescriptorSet descriptorSet,
        const std::function<void(VkCommandBuffer)>& additionalRecorder
    );
    void Submit(VkQueue graphicsQueue, uint32_t imageIndex);
    VkResult Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex);

private:
    void CreateCommandPool(const QueueFamilyIndices& queueFamilies);
    void AllocateCommandBuffers(size_t commandBufferCount);
    void CreateSyncObjects();

    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    VkSemaphore m_imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence m_inFlightFence = VK_NULL_HANDLE;
};
