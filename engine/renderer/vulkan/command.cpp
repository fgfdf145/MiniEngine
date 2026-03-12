#include "command.h"

#include <array>

VulkanCommandContext::VulkanCommandContext(VkDevice device, const QueueFamilyIndices& queueFamilies, size_t commandBufferCount)
    : m_device(device)
{
    CreateCommandPool(queueFamilies);
    AllocateCommandBuffers(commandBufferCount);
    CreateSyncObjects(commandBufferCount);
}

VulkanCommandContext::~VulkanCommandContext()
{
    for (const VulkanFrameSyncObjects& frameSyncObjects : m_frameSyncObjects)
    {
        if (frameSyncObjects.inFlightFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(m_device, frameSyncObjects.inFlightFence, nullptr);
        }
        if (frameSyncObjects.renderFinishedSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, frameSyncObjects.renderFinishedSemaphore, nullptr);
        }
        if (frameSyncObjects.imageAvailableSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, frameSyncObjects.imageAvailableSemaphore, nullptr);
        }
    }
    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    }
}

VkResult VulkanCommandContext::AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex)
{
    VulkanFrameSyncObjects& currentFrameSyncObjects = m_frameSyncObjects[m_currentFrame];
    CheckVulkan(
        vkWaitForFences(m_device, 1, &currentFrameSyncObjects.inFlightFence, VK_TRUE, UINT64_MAX),
        "Failed waiting for in-flight fence"
    );

    const VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        swapchain,
        UINT64_MAX,
        currentFrameSyncObjects.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );

    if ((acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR) &&
        m_imagesInFlight[imageIndex] != VK_NULL_HANDLE)
    {
        CheckVulkan(vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX), "Failed waiting for image fence");
    }

    if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)
    {
        m_imagesInFlight[imageIndex] = currentFrameSyncObjects.inFlightFence;
    }

    return acquireResult;
}

void VulkanCommandContext::RecordCommandBuffer(
    uint32_t imageIndex,
    VkRenderPass renderPass,
    VkFramebuffer framebuffer,
    VkExtent2D extent,
    VkPipeline graphicsPipeline,
    VkPipelineLayout pipelineLayout,
    const std::vector<VulkanDrawItem>& drawItems,
    const std::function<void(VkCommandBuffer)>& additionalRecorder
)
{
    CheckVulkan(vkResetCommandBuffer(m_commandBuffers[imageIndex], 0), "Failed to reset command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CheckVulkan(vkBeginCommandBuffer(m_commandBuffers[imageIndex], &beginInfo), "Failed to begin command buffer");

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { { 0.08f, 0.1f, 0.16f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffer;
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(m_commandBuffers[imageIndex], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(m_commandBuffers[imageIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

    for (const VulkanDrawItem& drawItem : drawItems)
    {
        const VkBuffer vertexBuffers[] = { drawItem.vertexBuffer };
        const VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(m_commandBuffers[imageIndex], 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(m_commandBuffers[imageIndex], drawItem.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(
            m_commandBuffers[imageIndex],
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &drawItem.descriptorSet,
            0,
            nullptr
        );
        vkCmdPushConstants(
            m_commandBuffers[imageIndex],
            pipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(MaterialPushConstants),
            &drawItem.material
        );
        vkCmdDrawIndexed(m_commandBuffers[imageIndex], drawItem.indexCount, 1, 0, 0, 0);
    }

    if (additionalRecorder)
    {
        additionalRecorder(m_commandBuffers[imageIndex]);
    }

    vkCmdEndRenderPass(m_commandBuffers[imageIndex]);

    CheckVulkan(vkEndCommandBuffer(m_commandBuffers[imageIndex]), "Failed to end command buffer");
}

void VulkanCommandContext::Submit(VkQueue graphicsQueue, uint32_t imageIndex)
{
    VulkanFrameSyncObjects& currentFrameSyncObjects = m_frameSyncObjects[m_currentFrame];
    CheckVulkan(vkResetFences(m_device, 1, &currentFrameSyncObjects.inFlightFence), "Failed resetting in-flight fence");

    const VkSemaphore waitSemaphores[] = { currentFrameSyncObjects.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    const VkSemaphore signalSemaphores[] = { currentFrameSyncObjects.renderFinishedSemaphore };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    CheckVulkan(
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, currentFrameSyncObjects.inFlightFence),
        "Failed to submit draw command buffer"
    );
}

VkResult VulkanCommandContext::Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex)
{
    const VkSemaphore signalSemaphores[] = { m_frameSyncObjects[m_currentFrame].renderFinishedSemaphore };

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);
    m_currentFrame = (m_currentFrame + 1) % static_cast<uint32_t>(m_frameSyncObjects.size());
    return presentResult;
}

void VulkanCommandContext::CreateCommandPool(const QueueFamilyIndices& queueFamilies)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilies.graphicsFamily.value();

    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "Failed to create command pool");
}

void VulkanCommandContext::AllocateCommandBuffers(size_t commandBufferCount)
{
    m_commandBuffers.resize(commandBufferCount);

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, m_commandBuffers.data()), "Failed to allocate command buffers");
}

void VulkanCommandContext::CreateSyncObjects(size_t swapchainImageCount)
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    m_frameSyncObjects.resize(kMaxFramesInFlight);
    for (VulkanFrameSyncObjects& frameSyncObjects : m_frameSyncObjects)
    {
        CheckVulkan(
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frameSyncObjects.imageAvailableSemaphore),
            "Failed to create image available semaphore"
        );
        CheckVulkan(
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frameSyncObjects.renderFinishedSemaphore),
            "Failed to create render finished semaphore"
        );
        CheckVulkan(
            vkCreateFence(m_device, &fenceInfo, nullptr, &frameSyncObjects.inFlightFence),
            "Failed to create in-flight fence"
        );
    }

    m_imagesInFlight.assign(swapchainImageCount, VK_NULL_HANDLE);
}
