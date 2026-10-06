#pragma once

#include "common.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace me
{

// Records draws into secondary command buffers on the task system's threads, for a pass's primary
// command buffer to execute in order. Each thread records with a command pool of its own (a pool is
// used by one thread at a time), one set of pools per frame slot, reset when the slot comes round.
class VulkanParallelRecorder
{
  public:
    // A run of items one render pass draws: recorded in ranges of itemsPerBuffer, one secondary
    // command buffer each, which `buffers` holds in range order once Record returns.
    struct Batch
    {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t itemCount = 0;
        // Records items [begin, end) into a begun secondary command buffer: everything it draws
        // with included, as a secondary inherits no state.
        std::function<void(VkCommandBuffer commandBuffer, uint32_t begin, uint32_t end)> record;
        std::vector<VkCommandBuffer> buffers;
    };

    VulkanParallelRecorder(VkDevice device, uint32_t queueFamily, uint32_t frameSlots);
    ~VulkanParallelRecorder();
    VulkanParallelRecorder(const VulkanParallelRecorder&) = delete;
    VulkanParallelRecorder& operator=(const VulkanParallelRecorder&) = delete;

    // The frame slot recorded next, once its fence has signalled: every buffer its pools gave out
    // before is free again.
    void BeginFrame(uint32_t frameSlot);
    // Records every batch, all their ranges at once on the task system.
    void Record(std::span<Batch> batches, uint32_t itemsPerBuffer);

  private:
    struct ThreadPool
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> buffers;
        // Buffers handed out since the last reset.
        size_t used = 0;
    };
    // The calling thread's next free buffer in the current slot.
    VkCommandBuffer Acquire();

    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    // [frame slot][task thread]; a pool is made the first time its thread records.
    std::vector<std::vector<ThreadPool>> m_pools;
    uint32_t m_frameSlot = 0;
};
}
