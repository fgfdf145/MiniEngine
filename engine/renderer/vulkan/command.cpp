#include "command.h"

VulkanCommandContext::VulkanCommandContext(
    VkDevice device,
    const QueueFamilyIndices& queueFamilies,
    VkRenderPass renderPass,
    VkPipeline graphicsPipeline,
    VkExtent2D extent,
    VkBuffer vertexBuffer,
    uint32_t vertexCount,
    const std::vector<VkFramebuffer>& framebuffers
)
    : m_device(device)
{
    CreateCommandPool(queueFamilies);
    AllocateCommandBuffers(framebuffers.size());
    RecordCommandBuffers(renderPass, graphicsPipeline, extent, vertexBuffer, vertexCount, framebuffers);
    CreateSyncObjects();
}

VulkanCommandContext::~VulkanCommandContext()
{
    if (m_inFlightFence != VK_NULL_HANDLE)
    {
        vkDestroyFence(m_device, m_inFlightFence, nullptr);
    }
    if (m_renderFinishedSemaphore != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(m_device, m_renderFinishedSemaphore, nullptr);
    }
    if (m_imageAvailableSemaphore != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(m_device, m_imageAvailableSemaphore, nullptr);
    }
    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    }
}

VkResult VulkanCommandContext::AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex)
{
    CheckVulkan(vkWaitForFences(m_device, 1, &m_inFlightFence, VK_TRUE, UINT64_MAX), "Failed waiting for in-flight fence");
    CheckVulkan(vkResetFences(m_device, 1, &m_inFlightFence), "Failed resetting in-flight fence");

    return vkAcquireNextImageKHR(
        m_device,
        swapchain,
        UINT64_MAX,
        m_imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );
}

void VulkanCommandContext::Submit(VkQueue graphicsQueue, uint32_t imageIndex)
{
    const VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    const VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphore };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    CheckVulkan(vkQueueSubmit(graphicsQueue, 1, &submitInfo, m_inFlightFence), "Failed to submit draw command buffer");
}

VkResult VulkanCommandContext::Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex)
{
    const VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphore };

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    return vkQueuePresentKHR(presentQueue, &presentInfo);
}

void VulkanCommandContext::CreateCommandPool(const QueueFamilyIndices& queueFamilies)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
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

void VulkanCommandContext::RecordCommandBuffers(
    VkRenderPass renderPass,
    VkPipeline graphicsPipeline,
    VkExtent2D extent,
    VkBuffer vertexBuffer,
    uint32_t vertexCount,
    const std::vector<VkFramebuffer>& framebuffers
)
{
    for (size_t i = 0; i < m_commandBuffers.size(); ++i)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        CheckVulkan(vkBeginCommandBuffer(m_commandBuffers[i], &beginInfo), "Failed to begin command buffer");

        VkClearValue clearColor{};
        clearColor.color = { { 0.08f, 0.1f, 0.16f, 1.0f } };

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = framebuffers[i];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = extent;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(m_commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(m_commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        const VkBuffer vertexBuffers[] = { vertexBuffer };
        const VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(m_commandBuffers[i], 0, 1, vertexBuffers, offsets);
        vkCmdDraw(m_commandBuffers[i], vertexCount, 1, 0, 0);
        vkCmdEndRenderPass(m_commandBuffers[i]);

        CheckVulkan(vkEndCommandBuffer(m_commandBuffers[i]), "Failed to end command buffer");
    }
}

void VulkanCommandContext::CreateSyncObjects()
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    CheckVulkan(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphore), "Failed to create image available semaphore");
    CheckVulkan(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphore), "Failed to create render finished semaphore");
    CheckVulkan(vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFence), "Failed to create in-flight fence");
}
