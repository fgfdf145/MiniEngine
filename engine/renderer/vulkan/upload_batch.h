#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace me
{

// Commands recorded straight into a native Vulkan command buffer and run at once: what the NGX
// Vulkan entry points that take a command buffer of their own need. Flush submits and waits.
class VulkanImmediateCommands
{
  public:
    VulkanImmediateCommands(VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue);
    ~VulkanImmediateCommands();

    VulkanImmediateCommands(const VulkanImmediateCommands&) = delete;
    VulkanImmediateCommands& operator=(const VulkanImmediateCommands&) = delete;

    VkCommandBuffer GetCommandBuffer();
    // Submits what was recorded and waits for the queue. Recording can go on afterwards.
    void Flush();

  private:
    void BeginRecording();

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    bool m_hasCommands = false;
};

// Upload command lists kept between batches. A command list keeps NVRHI's staging chunks with it and
// reuses them once the GPU has read them, so a list taken from here stages without allocating: making
// a 32 MB mapped staging chunk cost about a millisecond of every batch, and a streamed map stages
// textures in nearly every frame while it loads. Holds at most kMaxLists.
class GpuUploadPool
{
  public:
    static constexpr size_t kMaxLists = 3;
    // NVRHI's staging chunk size for the pool's lists; a larger write gets a chunk of its own.
    static constexpr uint64_t kChunkBytes = uint64_t{32} << 20;

    explicit GpuUploadPool(nvrhi::IDevice* device);

    GpuUploadPool(const GpuUploadPool&) = delete;
    GpuUploadPool& operator=(const GpuUploadPool&) = delete;

    // A kept list, or a new one.
    nvrhi::CommandListHandle Take();
    // Keeps a list the GPU has finished with; dropped when the pool is full.
    void Give(nvrhi::CommandListHandle list);

  private:
    nvrhi::IDevice* m_device = nullptr;
    std::mutex m_mutex;
    std::vector<nvrhi::CommandListHandle> m_lists;
};

// Accumulates uploads (buffer and texture writes, which NVRHI stages) from many resources into one
// command list, so the caller submits once instead of once per resource: loading a model with
// hundreds of submeshes and textures (Sponza) one round trip at a time took seconds. The list runs
// with NVRHI's automatic barriers: each write moves its resource to a copy destination, and closing
// the list moves every one back to its resting state (keepInitialState), ordering the copies before
// everything submitted after.
//
// Destroying a batch without a Flush or SubmitWithoutWait discards whatever was recorded since: the
// commands never reach the GPU. That is the unwind path of an upload that failed part way.
class VulkanUploadBatch
{
  public:
    // pool, when given, lends the batch its command list (GpuUploadPool).
    explicit VulkanUploadBatch(nvrhi::IDevice* device, GpuUploadPool* pool = nullptr);
    ~VulkanUploadBatch();

    VulkanUploadBatch(const VulkanUploadBatch&) = delete;
    VulkanUploadBatch& operator=(const VulkanUploadBatch&) = delete;

    // The open command list: anything recorded into it is submitted by the next Flush.
    nvrhi::ICommandList* GetCommandList();
    void WriteBuffer(nvrhi::IBuffer* buffer, const void* data, uint64_t byteSize, uint64_t offset = 0);
    // One level of the first array slice: rowPitch bytes a row of texels (of blocks, for a block
    // format), byteSize in all.
    void WriteTexture(nvrhi::ITexture* texture, uint32_t mipLevel, const void* data, uint64_t rowPitch, uint64_t byteSize);
    // Bytes written since the last Flush: callers flush on this rather than on a count of resources.
    uint64_t StagedBytes() const;

    // Submits everything recorded so far, waits for the GPU to finish, and re-arms the batch. No-op
    // when nothing was recorded since the last Flush.
    void Flush();
    // Submits everything recorded so far without waiting; nothing more can be recorded. The batch
    // keeps its command list (and NVRHI its staging) until IsComplete; the destructor waits for a
    // batch still running.
    void SubmitWithoutWait();
    bool IsComplete() const;
    // Nothing recorded since the last Flush.
    bool IsEmpty() const;

  private:
    nvrhi::IDevice* m_device = nullptr;
    GpuUploadPool* m_pool = nullptr;
    nvrhi::CommandListHandle m_commandList;
    nvrhi::EventQueryHandle m_query;
    bool m_open = false;
    bool m_hasCommands = false;
    bool m_submitted = false;
    uint64_t m_stagedBytes = 0;
};
}
