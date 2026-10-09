#include "upload_batch.h"

#include <stdexcept>
#include <utility>

namespace me
{

VulkanImmediateCommands::VulkanImmediateCommands(VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue)
    : m_device(device), m_graphicsQueue(graphicsQueue)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "Failed to create an immediate command pool");

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, &m_commandBuffer), "Failed to allocate an immediate command buffer");
    BeginRecording();
}

VulkanImmediateCommands::~VulkanImmediateCommands()
{
    // Whatever was recorded since the last Flush never reached the GPU.
    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    }
}

VkCommandBuffer VulkanImmediateCommands::GetCommandBuffer()
{
    m_hasCommands = true;
    return m_commandBuffer;
}

void VulkanImmediateCommands::BeginRecording()
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVulkan(vkBeginCommandBuffer(m_commandBuffer, &beginInfo), "Failed to begin an immediate command buffer");
}

void VulkanImmediateCommands::Flush()
{
    if (!m_hasCommands)
    {
        return;
    }
    CheckVulkan(vkEndCommandBuffer(m_commandBuffer), "Failed to end an immediate command buffer");
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    CheckVulkan(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit immediate commands");
    CheckVulkan(vkQueueWaitIdle(m_graphicsQueue), "Failed to wait for immediate commands");
    m_hasCommands = false;
    CheckVulkan(vkResetCommandPool(m_device, m_commandPool, 0), "Failed to reset an immediate command pool");
    BeginRecording();
}

GpuUploadPool::GpuUploadPool(nvrhi::IDevice* device)
    : m_device(device)
{
}

nvrhi::CommandListHandle GpuUploadPool::Take()
{
    {
        const std::lock_guard lock(m_mutex);
        if (!m_lists.empty())
        {
            nvrhi::CommandListHandle list = std::move(m_lists.back());
            m_lists.pop_back();
            return list;
        }
    }
    nvrhi::CommandListParameters parameters;
    parameters.setUploadChunkSize(kChunkBytes);
    nvrhi::CommandListHandle list = m_device->createCommandList(parameters);
    if (!list)
    {
        throw std::runtime_error("Failed to create an upload command list");
    }
    return list;
}

void GpuUploadPool::Give(nvrhi::CommandListHandle list)
{
    const std::lock_guard lock(m_mutex);
    if (m_lists.size() < kMaxLists)
    {
        m_lists.push_back(std::move(list));
    }
}

VulkanUploadBatch::VulkanUploadBatch(nvrhi::IDevice* device, GpuUploadPool* pool)
    : m_device(device), m_pool(pool)
{
}

VulkanUploadBatch::~VulkanUploadBatch()
{
    if (m_submitted)
    {
        // Submitted without a wait: its staging is read until the query signals.
        m_device->waitEventQuery(m_query);
        if (m_pool != nullptr)
        {
            m_pool->Give(std::move(m_commandList));
        }
        return;
    }
    if (m_open)
    {
        // Abandoned: the list is closed and dropped, never executed (and never lent out again).
        m_commandList->close();
    }
    else if (m_commandList && m_pool != nullptr)
    {
        m_pool->Give(std::move(m_commandList));
    }
}

nvrhi::ICommandList* VulkanUploadBatch::GetCommandList()
{
    if (m_submitted)
    {
        throw std::logic_error("An upload batch submitted without a wait takes no more commands");
    }
    if (!m_open)
    {
        if (!m_commandList)
        {
            if (m_pool != nullptr)
            {
                m_commandList = m_pool->Take();
            }
            else
            {
                nvrhi::CommandListParameters parameters;
                parameters.setUploadChunkSize(GpuUploadPool::kChunkBytes);
                m_commandList = m_device->createCommandList(parameters);
                if (!m_commandList)
                {
                    throw std::runtime_error("Failed to create an upload command list");
                }
            }
        }
        m_commandList->open();
        m_open = true;
    }
    m_hasCommands = true;
    return m_commandList;
}

void VulkanUploadBatch::WriteBuffer(nvrhi::IBuffer* buffer, const void* data, uint64_t byteSize, uint64_t offset)
{
    GetCommandList()->writeBuffer(buffer, data, static_cast<size_t>(byteSize), offset);
    m_stagedBytes += byteSize;
}

void VulkanUploadBatch::WriteTexture(nvrhi::ITexture* texture, uint32_t mipLevel, const void* data, uint64_t rowPitch, uint64_t byteSize)
{
    GetCommandList()->writeTexture(texture, 0, mipLevel, data, static_cast<size_t>(rowPitch), static_cast<size_t>(byteSize));
    m_stagedBytes += byteSize;
}

uint64_t VulkanUploadBatch::StagedBytes() const
{
    return m_stagedBytes;
}

void VulkanUploadBatch::Flush()
{
    if (!m_open)
    {
        return;
    }
    // One submit and one wait for everything recorded since the last Flush, instead of a
    // submit-and-stall per resource.
    m_commandList->close();
    m_open = false;
    m_device->executeCommandList(m_commandList);
    m_device->waitForIdle();
    m_hasCommands = false;
    m_stagedBytes = 0;
}

void VulkanUploadBatch::SubmitWithoutWait()
{
    if (m_submitted)
    {
        throw std::logic_error("An upload batch is submitted without a wait only once");
    }
    if (!m_open)
    {
        return;
    }
    m_commandList->close();
    m_open = false;
    m_device->executeCommandList(m_commandList);
    m_query = m_device->createEventQuery();
    m_device->setEventQuery(m_query, nvrhi::CommandQueue::Graphics);
    m_submitted = true;
    m_hasCommands = false;
}

bool VulkanUploadBatch::IsComplete() const
{
    return !m_submitted || m_device->pollEventQuery(m_query);
}

bool VulkanUploadBatch::IsEmpty() const
{
    return !m_hasCommands;
}
}
