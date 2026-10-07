#include "command.h"

#include <algorithm>

namespace me
{

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
        if (frameSyncObjects.imageAvailableSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, frameSyncObjects.imageAvailableSemaphore, nullptr);
        }
    }
    for (VkSemaphore renderFinishedSemaphore : m_renderFinishedSemaphores)
    {
        if (renderFinishedSemaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, renderFinishedSemaphore, nullptr);
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
        "Failed waiting for in-flight fence");
    m_completedSubmits = std::max(m_completedSubmits, m_slotSubmits[m_currentFrame]);

    const VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        swapchain,
        UINT64_MAX,
        currentFrameSyncObjects.imageAvailableSemaphore,
        VK_NULL_HANDLE,
        &imageIndex);

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

void VulkanCommandContext::RecordCommandBuffer(uint32_t imageIndex, const std::function<void(VkCommandBuffer)>& recorder)
{
    CheckVulkan(vkResetCommandBuffer(m_commandBuffers[imageIndex], 0), "Failed to reset command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CheckVulkan(vkBeginCommandBuffer(m_commandBuffers[imageIndex], &beginInfo), "Failed to begin command buffer");

    if (recorder)
    {
        recorder(m_commandBuffers[imageIndex]);
    }

    CheckVulkan(vkEndCommandBuffer(m_commandBuffers[imageIndex]), "Failed to end command buffer");
}

void VulkanCommandContext::Submit(VkQueue graphicsQueue, uint32_t imageIndex)
{
    VulkanFrameSyncObjects& currentFrameSyncObjects = m_frameSyncObjects[m_currentFrame];
    CheckVulkan(vkResetFences(m_device, 1, &currentFrameSyncObjects.inFlightFence), "Failed resetting in-flight fence");

    const VkSemaphore waitSemaphores[] = {currentFrameSyncObjects.imageAvailableSemaphore};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[imageIndex]};

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
        "Failed to submit draw command buffer");
    m_slotSubmits[m_currentFrame] = ++m_lastSubmit;
}

VkResult VulkanCommandContext::Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex)
{
    const VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[imageIndex]};

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

uint32_t VulkanCommandContext::GetCurrentFrame() const
{
    return m_currentFrame;
}

void VulkanCommandContext::WaitForAllFrames()
{
    std::vector<VkFence> fences;
    fences.reserve(m_frameSyncObjects.size());
    for (const VulkanFrameSyncObjects& syncObjects : m_frameSyncObjects)
    {
        fences.push_back(syncObjects.inFlightFence);
    }
    if (!fences.empty())
    {
        CheckVulkan(
            vkWaitForFences(m_device, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, UINT64_MAX),
            "Failed waiting for in-flight render fences");
    }
    m_completedSubmits = m_lastSubmit;
}

uint64_t VulkanCommandContext::LastSubmit() const
{
    return m_lastSubmit;
}

uint64_t VulkanCommandContext::CompletedSubmits()
{
    // A slot's fence covers its own submit and, the queue running in order, every submit before it.
    for (size_t slot = 0; slot < m_frameSyncObjects.size(); ++slot)
    {
        if (m_slotSubmits[slot] > m_completedSubmits && vkGetFenceStatus(m_device, m_frameSyncObjects[slot].inFlightFence) == VK_SUCCESS)
        {
            m_completedSubmits = m_slotSubmits[slot];
        }
    }
    return m_completedSubmits;
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
    m_slotSubmits.assign(kMaxFramesInFlight, 0);
    for (VulkanFrameSyncObjects& frameSyncObjects : m_frameSyncObjects)
    {
        CheckVulkan(
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &frameSyncObjects.imageAvailableSemaphore),
            "Failed to create image available semaphore");
        CheckVulkan(
            vkCreateFence(m_device, &fenceInfo, nullptr, &frameSyncObjects.inFlightFence),
            "Failed to create in-flight fence");
    }

    m_renderFinishedSemaphores.resize(swapchainImageCount, VK_NULL_HANDLE);
    for (VkSemaphore& renderFinishedSemaphore : m_renderFinishedSemaphores)
    {
        CheckVulkan(
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &renderFinishedSemaphore),
            "Failed to create render finished semaphore");
    }

    m_imagesInFlight.assign(swapchainImageCount, VK_NULL_HANDLE);
}
}
