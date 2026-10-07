#pragma once

#include "common.h"

#include <utility>
#include <vector>

namespace me
{

// Accumulates GPU upload commands (buffer-to-buffer and buffer-to-image copies, image layout
// transitions) from many resource uploads into a single command buffer, so the caller submits
// and waits once instead of once per resource. Used by VulkanBuffer and VulkanTexture so that
// loading a model with hundreds of submeshes/textures (e.g. Sponza) doesn't serialize hundreds
// of individual GPU round-trips. Flush() resets the batch so it can keep being reused.
//
// Destroying a batch without a final Flush() discards whatever was recorded since the last one:
// the commands are never submitted, and the staging buffers tracked for them are freed. That is
// the unwind path of an upload that failed part way, and it is safe precisely because nothing
// recorded since the last Flush() ever reached the GPU.
class VulkanUploadBatch
{
  public:
    VulkanUploadBatch(VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue);
    // With the physical device the batch can also stage (Stage), out of a few large mapped chunks
    // instead of one allocation per resource.
    VulkanUploadBatch(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue);
    ~VulkanUploadBatch();

    VulkanUploadBatch(const VulkanUploadBatch&) = delete;
    VulkanUploadBatch& operator=(const VulkanUploadBatch&) = delete;

    // Anything recorded into it is submitted by the next Flush(), whether or not it tracked a
    // staging buffer: a readback records commands with nothing to stage.
    VkCommandBuffer GetCommandBuffer();
    // Takes ownership. Track a staging buffer as soon as it exists, before anything else that can
    // throw, so a later failure in the same upload cannot leak it. Either handle may be null.
    void TrackStagingResource(VkBuffer buffer, VkDeviceMemory memory);

    // Where Stage put the bytes: copy from (buffer, offset) in a command recorded into this batch.
    struct StagingSlice
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    bool CanStage() const;
    // Copies size bytes into the batch's staging memory, which lives until the next Flush(). Chunks of
    // kStagingChunkBytes (or one as large as a bigger request) are made as needed and mapped once.
    StagingSlice Stage(const void* data, VkDeviceSize size, VkDeviceSize alignment = 16);
    // Bytes staged since the last Flush(): callers flush on this rather than on a count of resources.
    VkDeviceSize StagedBytes() const;
    static constexpr VkDeviceSize kStagingChunkBytes = VkDeviceSize{32} << 20;

    // Submits everything recorded so far, waits for the GPU to finish, frees the staging
    // buffers tracked since the last Flush(), and re-arms the batch for more recording.
    // No-op if nobody asked for the command buffer since the last Flush().
    void Flush();
    // Submits everything recorded so far without waiting: a closing barrier orders the copies (and
    // the layout changes) before every later submission to the queue, and the batch keeps its
    // staging memory and command buffer until IsComplete. Nothing more can be recorded into it. Flush
    // waits for the whole queue, frames in flight included: a frame that staged streamed textures
    // spent ~10 ms of the frame's thread in it. The destructor waits for a batch still running.
    void SubmitWithoutWait();
    bool IsComplete() const;

  private:
    void BeginRecording();

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_stagingResources;
    struct StagingChunk
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        unsigned char* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    void ReleaseStagingChunks();
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    std::vector<StagingChunk> m_stagingChunks;
    VkDeviceSize m_stagedBytes = 0;
    bool m_hasCommands = false;
    // Set by SubmitWithoutWait.
    VkFence m_fence = VK_NULL_HANDLE;
};
}
