#include "command.h"

#include "nvrhi_device.h"

#include <algorithm>

namespace me
{

namespace
{
VkSemaphore CreateBinarySemaphore(VkDevice device, const char* what)
{
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    CheckVulkan(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), what);
    return semaphore;
}
}

VulkanCommandContext::VulkanCommandContext(NvrhiDevice& nvrhi, VkDevice device, size_t swapchainImageCount)
    : m_nvrhi(nvrhi), m_device(device)
{
    m_commandList = m_nvrhi.Get()->createCommandList();
    m_imageAvailableSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
    m_frameQueries.resize(kMaxFramesInFlight);
    m_slotSubmits.assign(kMaxFramesInFlight, 0);
    for (size_t slot = 0; slot < kMaxFramesInFlight; ++slot)
    {
        m_imageAvailableSemaphores[slot] = CreateBinarySemaphore(m_device, "Failed to create image available semaphore");
        m_frameQueries[slot] = m_nvrhi.Get()->createEventQuery();
    }
    m_renderFinishedSemaphores.resize(swapchainImageCount, VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : m_renderFinishedSemaphores)
    {
        semaphore = CreateBinarySemaphore(m_device, "Failed to create render finished semaphore");
    }
    m_imagesInFlight.assign(swapchainImageCount, -1);
}

VulkanCommandContext::~VulkanCommandContext()
{
    WaitForAllFrames();
    m_frameQueries.clear();
    m_commandList = nullptr;
    for (VkSemaphore semaphore : m_imageAvailableSemaphores)
    {
        if (semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
    for (VkSemaphore semaphore : m_renderFinishedSemaphores)
    {
        if (semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, semaphore, nullptr);
        }
    }
}

VkResult VulkanCommandContext::AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex)
{
    if (m_slotSubmits[m_currentFrame] != 0)
    {
        m_nvrhi.Get()->waitEventQuery(m_frameQueries[m_currentFrame]);
        m_completedSubmits = std::max(m_completedSubmits, m_slotSubmits[m_currentFrame]);
    }

    const VkResult acquireResult = vkAcquireNextImageKHR(
        m_device,
        swapchain,
        UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrame],
        VK_NULL_HANDLE,
        &imageIndex);

    if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)
    {
        // The image's previous frame, if another slot drew it, must be done with it.
        const int previousSlot = m_imagesInFlight[imageIndex];
        if (previousSlot >= 0 && static_cast<uint32_t>(previousSlot) != m_currentFrame && m_slotSubmits[previousSlot] != 0)
        {
            m_nvrhi.Get()->waitEventQuery(m_frameQueries[previousSlot]);
            m_completedSubmits = std::max(m_completedSubmits, m_slotSubmits[previousSlot]);
        }
        m_imagesInFlight[imageIndex] = static_cast<int>(m_currentFrame);
    }

    return acquireResult;
}

void VulkanCommandContext::RecordCommandBuffer(uint32_t imageIndex, const std::function<void(VkCommandBuffer)>& recorder)
{
    (void)imageIndex;
    m_commandList->open();
    if (recorder)
    {
        const VkCommandBuffer commandBuffer = m_commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        recorder(commandBuffer);
    }
    m_commandList->close();
}

void VulkanCommandContext::Submit(VkQueue graphicsQueue, uint32_t imageIndex)
{
    (void)graphicsQueue;
    nvrhi::vulkan::IDevice* vulkan = m_nvrhi.GetVulkan();
    vulkan->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, m_imageAvailableSemaphores[m_currentFrame], 0);
    vulkan->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, m_renderFinishedSemaphores[imageIndex], 0);
    nvrhi::IDevice* device = m_nvrhi.Get();
    device->executeCommandList(m_commandList);
    device->resetEventQuery(m_frameQueries[m_currentFrame]);
    device->setEventQuery(m_frameQueries[m_currentFrame], nvrhi::CommandQueue::Graphics);
    m_slotSubmits[m_currentFrame] = ++m_lastSubmit;
}

VkResult VulkanCommandContext::Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex)
{
    const VkSemaphore waitSemaphores[] = {m_renderFinishedSemaphores[imageIndex]};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = waitSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(presentQueue, &presentInfo);
    m_currentFrame = (m_currentFrame + 1) % static_cast<uint32_t>(kMaxFramesInFlight);
    // Command buffers and resources NVRHI kept for frames that have finished.
    m_nvrhi.Get()->runGarbageCollection();
    return presentResult;
}

uint32_t VulkanCommandContext::GetCurrentFrame() const
{
    return m_currentFrame;
}

void VulkanCommandContext::WaitForAllFrames()
{
    for (size_t slot = 0; slot < m_frameQueries.size(); ++slot)
    {
        if (m_slotSubmits[slot] != 0)
        {
            m_nvrhi.Get()->waitEventQuery(m_frameQueries[slot]);
        }
    }
    m_completedSubmits = m_lastSubmit;
}

uint64_t VulkanCommandContext::LastSubmit() const
{
    return m_lastSubmit;
}

uint64_t VulkanCommandContext::CompletedSubmits()
{
    // A slot's query covers its own submit and, the queue running in order, every submit before it.
    for (size_t slot = 0; slot < m_frameQueries.size(); ++slot)
    {
        if (m_slotSubmits[slot] > m_completedSubmits && m_nvrhi.Get()->pollEventQuery(m_frameQueries[slot]))
        {
            m_completedSubmits = m_slotSubmits[slot];
        }
    }
    return m_completedSubmits;
}
}
