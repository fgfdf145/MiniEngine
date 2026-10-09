#include "command.h"

#include "gpu_device.h"

#include <algorithm>

namespace me
{

VulkanCommandContext::VulkanCommandContext(nvrhi::IDevice* device, GpuSwapchain& swapchain)
    : m_device(device), m_swapchain(swapchain)
{
    m_commandList = m_device->createCommandList();
    m_frameQueries.resize(kMaxFramesInFlight);
    m_slotSubmits.assign(kMaxFramesInFlight, 0);
    for (nvrhi::EventQueryHandle& query : m_frameQueries)
    {
        query = m_device->createEventQuery();
    }
    m_imagesInFlight.assign(swapchain.GetImageCount(), -1);
}

VulkanCommandContext::~VulkanCommandContext()
{
    WaitForAllFrames();
    m_frameQueries.clear();
    m_commandList = nullptr;
}

SwapchainStatus VulkanCommandContext::AcquireNextImage(uint32_t& imageIndex)
{
    if (m_slotSubmits[m_currentFrame] != 0)
    {
        m_device->waitEventQuery(m_frameQueries[m_currentFrame]);
        m_completedSubmits = std::max(m_completedSubmits, m_slotSubmits[m_currentFrame]);
    }

    const SwapchainStatus status = m_swapchain.Acquire(m_currentFrame, imageIndex);
    if (status != SwapchainStatus::OutOfDate)
    {
        // The image's previous frame, if another slot drew it, must be done with it.
        const int previousSlot = m_imagesInFlight[imageIndex];
        if (previousSlot >= 0 && static_cast<uint32_t>(previousSlot) != m_currentFrame && m_slotSubmits[previousSlot] != 0)
        {
            m_device->waitEventQuery(m_frameQueries[previousSlot]);
            m_completedSubmits = std::max(m_completedSubmits, m_slotSubmits[previousSlot]);
        }
        m_imagesInFlight[imageIndex] = static_cast<int>(m_currentFrame);
    }
    return status;
}

nvrhi::ICommandList* VulkanCommandContext::GetCommandList() const
{
    return m_commandList;
}

void VulkanCommandContext::RecordCommandBuffer(uint32_t imageIndex, const std::function<void(VkCommandBuffer)>& recorder)
{
    (void)imageIndex;
    m_commandList->open();
    // The passes set their states themselves (NvrhiPassScope).
    m_commandList->setEnableAutomaticBarriers(false);
    if (recorder)
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (m_device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            commandBuffer = m_commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        }
        recorder(commandBuffer);
    }
    m_commandList->close();
}

void VulkanCommandContext::Submit(uint32_t imageIndex)
{
    m_swapchain.BeforeSubmit(m_currentFrame, imageIndex);
    m_device->executeCommandList(m_commandList);
    m_device->resetEventQuery(m_frameQueries[m_currentFrame]);
    m_device->setEventQuery(m_frameQueries[m_currentFrame], nvrhi::CommandQueue::Graphics);
    m_slotSubmits[m_currentFrame] = ++m_lastSubmit;
}

SwapchainStatus VulkanCommandContext::Present(uint32_t imageIndex)
{
    const SwapchainStatus status = m_swapchain.Present(imageIndex);
    m_currentFrame = (m_currentFrame + 1) % static_cast<uint32_t>(kMaxFramesInFlight);
    // Command buffers and resources NVRHI kept for frames that have finished.
    m_device->runGarbageCollection();
    return status;
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
            m_device->waitEventQuery(m_frameQueries[slot]);
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
        if (m_slotSubmits[slot] > m_completedSubmits && m_device->pollEventQuery(m_frameQueries[slot]))
        {
            m_completedSubmits = m_slotSubmits[slot];
        }
    }
    return m_completedSubmits;
}
}
