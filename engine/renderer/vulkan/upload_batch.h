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
    ~VulkanUploadBatch();

    VulkanUploadBatch(const VulkanUploadBatch&) = delete;
    VulkanUploadBatch& operator=(const VulkanUploadBatch&) = delete;

    // Anything recorded into it is submitted by the next Flush(), whether or not it tracked a
    // staging buffer: a readback records commands with nothing to stage.
    VkCommandBuffer GetCommandBuffer();
    // Takes ownership. Track a staging buffer as soon as it exists, before anything else that can
    // throw, so a later failure in the same upload cannot leak it. Either handle may be null.
    void TrackStagingResource(VkBuffer buffer, VkDeviceMemory memory);

    // Submits everything recorded so far, waits for the GPU to finish, frees the staging
    // buffers tracked since the last Flush(), and re-arms the batch for more recording.
    // No-op if nobody asked for the command buffer since the last Flush().
    void Flush();

  private:
    void BeginRecording();

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_stagingResources;
    bool m_hasCommands = false;
};
}
