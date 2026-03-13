#pragma once

#include "common.h"

#include <functional>

struct VulkanDrawItem
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    ObjectPushConstants drawConstants;
};

struct VulkanFrameSyncObjects
{
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
};

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
        const std::vector<VulkanDrawItem>& drawItems,
        const std::function<void(VkCommandBuffer)>& additionalRecorder
    );
    void Submit(VkQueue graphicsQueue, uint32_t imageIndex);
    VkResult Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex);

private:
    void CreateCommandPool(const QueueFamilyIndices& queueFamilies);
    void AllocateCommandBuffers(size_t commandBufferCount);
    void CreateSyncObjects(size_t swapchainImageCount);

    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VulkanFrameSyncObjects> m_frameSyncObjects;
    std::vector<VkFence> m_imagesInFlight;
    uint32_t m_currentFrame = 0;

    static constexpr size_t kMaxFramesInFlight = 2;
};
