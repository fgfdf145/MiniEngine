#pragma once

#include "common.h"

#include <utility>
#include <vector>

// Accumulates GPU upload commands (buffer-to-buffer and buffer-to-image copies, image layout
// transitions) from many resource uploads into a single command buffer, so the caller submits
// and waits once instead of once per resource. Used by VulkanBuffer and VulkanTexture so that
// loading a model with hundreds of submeshes/textures (e.g. Sponza) doesn't serialize hundreds
// of individual GPU round-trips. Flush() resets the batch so it can keep being reused.
class VulkanUploadBatch
{
public:
    VulkanUploadBatch(VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue);
    ~VulkanUploadBatch();

    VulkanUploadBatch(const VulkanUploadBatch&) = delete;
    VulkanUploadBatch& operator=(const VulkanUploadBatch&) = delete;

    VkCommandBuffer GetCommandBuffer() const;
    void TrackStagingResource(VkBuffer buffer, VkDeviceMemory memory);

    // Submits everything recorded so far, waits for the GPU to finish, frees the staging
    // buffers tracked since the last Flush(), and re-arms the batch for more recording.
    // No-op if nothing has been recorded.
    void Flush();

private:
    void BeginRecording();

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_stagingResources;
};
