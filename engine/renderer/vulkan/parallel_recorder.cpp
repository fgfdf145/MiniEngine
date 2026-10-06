#include "parallel_recorder.h"

#include <engine/core/threading/task_system.h>

#include <algorithm>

namespace me
{

namespace
{
// The calling thread's index among the task system's threads; 0 when it has none (nothing runs in
// parallel then).
uint32_t CurrentTaskThread()
{
    if (!TaskSystem::CanWaitOnCurrentThread())
    {
        return 0;
    }
    return TaskSystem::Scheduler().GetThreadNum();
}
}

VulkanParallelRecorder::VulkanParallelRecorder(VkDevice device, uint32_t queueFamily, uint32_t frameSlots)
    : m_device(device),
      m_queueFamily(queueFamily),
      m_pools(frameSlots, std::vector<ThreadPool>(std::max(TaskSystem::ThreadCount(), 1u)))
{
}

VulkanParallelRecorder::~VulkanParallelRecorder()
{
    for (std::vector<ThreadPool>& slot : m_pools)
    {
        for (ThreadPool& threadPool : slot)
        {
            if (threadPool.pool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_device, threadPool.pool, nullptr);
            }
        }
    }
}

void VulkanParallelRecorder::BeginFrame(uint32_t frameSlot)
{
    m_frameSlot = frameSlot;
    for (ThreadPool& threadPool : m_pools[frameSlot])
    {
        if (threadPool.pool != VK_NULL_HANDLE && threadPool.used > 0)
        {
            CheckVulkan(vkResetCommandPool(m_device, threadPool.pool, 0), "Failed to reset a recording command pool");
        }
        threadPool.used = 0;
    }
}

void VulkanParallelRecorder::Record(std::span<Batch> batches, uint32_t itemsPerBuffer)
{
    // Every batch's ranges, numbered across the batches.
    std::vector<uint32_t> firstRange(batches.size() + 1, 0);
    for (size_t batch = 0; batch < batches.size(); ++batch)
    {
        const uint32_t ranges = (batches[batch].itemCount + itemsPerBuffer - 1) / itemsPerBuffer;
        batches[batch].buffers.assign(ranges, VK_NULL_HANDLE);
        firstRange[batch + 1] = firstRange[batch] + ranges;
    }
    TaskSystem::ParallelFor(firstRange.back(), 1, [&](uint32_t begin, uint32_t end)
                            {
                                for (uint32_t range = begin; range < end; ++range)
                                {
                                    const size_t batchIndex = static_cast<size_t>(std::upper_bound(firstRange.begin(), firstRange.end(), range) - firstRange.begin()) - 1;
                                    Batch& batch = batches[batchIndex];
                                    const uint32_t local = range - firstRange[batchIndex];
                                    const uint32_t first = local * itemsPerBuffer;
                                    const uint32_t last = std::min(first + itemsPerBuffer, batch.itemCount);

                                    VkCommandBufferInheritanceInfo inheritance{};
                                    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
                                    inheritance.renderPass = batch.renderPass;
                                    inheritance.subpass = 0;
                                    inheritance.framebuffer = batch.framebuffer;
                                    VkCommandBufferBeginInfo beginInfo{};
                                    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                                    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                    beginInfo.pInheritanceInfo = &inheritance;

                                    const VkCommandBuffer commandBuffer = Acquire();
                                    CheckVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin a secondary command buffer");
                                    batch.record(commandBuffer, first, last);
                                    CheckVulkan(vkEndCommandBuffer(commandBuffer), "Failed to end a secondary command buffer");
                                    batch.buffers[local] = commandBuffer;
                                }
                            });
}

VkCommandBuffer VulkanParallelRecorder::Acquire()
{
    ThreadPool& threadPool = m_pools[m_frameSlot][CurrentTaskThread()];
    if (threadPool.pool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Reset whole, per frame slot: the buffers are never reset one at a time.
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_queueFamily;
        CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &threadPool.pool), "Failed to create a recording command pool");
    }
    if (threadPool.used == threadPool.buffers.size())
    {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = threadPool.pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer), "Failed to allocate a secondary command buffer");
        threadPool.buffers.push_back(commandBuffer);
    }
    return threadPool.buffers[threadPool.used++];
}
}
