#pragma once

#include "common.h"
#include <engine/renderer/material_pipeline.h>
#include <engine/scene/toon_material.h>

#include <entt/entt.hpp>

#include <functional>

namespace me
{

struct VulkanDrawItem
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    ObjectPushConstants drawConstants;
    MaterialPipelineKey pipelineKey;
    // Index into the previous model matrix buffer (set 0 binding 2), passed to the draw as
    // firstInstance so triangle.vert can read it through gl_InstanceIndex.
    uint32_t motionSlot = 0;
    // An Opaque or Mask draw the forward pass shades (kShadingFlagForward).
    bool forwardShaded = false;
    // A transmissive Opaque or Mask draw (kShadingFlagTransmission): drawn only by the translucent
    // forward pass, never by the geometry pass or into shadow maps.
    bool transmissive = false;
    // Its material scatters (KHR_materials_volume_scatter, MaterialScatters): the scatter pre-pass
    // draws it as well.
    bool scatters = false;
    // A Blend item drawn as a deferred decal by the geometry pass (CpuRenderSubmesh::decal).
    bool decal = false;
    // An anime character material (RenderSubmesh::toon), shaded by the toon passes; it lives as long
    // as the render submesh the frame draws.
    const ToonMaterialData* toon = nullptr;
    // The entity drawn, and for a skinned toon face the palette entry of its head joint (-1 for none),
    // which the toon passes' head frame follows.
    entt::entity entity = entt::null;
    int32_t toonHeadJoint = -1;
    // Where each vertex was last frame (VulkanBuffer::GetPreviousPositionHandle): binding 1 of the
    // material pipelines, whose motion vectors start from it.
    VkBuffer previousPositionBuffer = VK_NULL_HANDLE;
};

struct VulkanFrameSyncObjects
{
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
};

class VulkanCommandContext
{
  public:
    // How many frames may be recorded before the oldest must complete. AcquireNextImage waits on
    // this slot's fence before acquiring, so any resource indexed by GetCurrentFrame() is free for
    // reuse once that call returns. SceneRenderTargets sizes its transient targets against this.
    static constexpr size_t kMaxFramesInFlight = 2;

    VulkanCommandContext(VkDevice device, const QueueFamilyIndices& queueFamilies, size_t commandBufferCount);
    ~VulkanCommandContext();

    VulkanCommandContext(const VulkanCommandContext&) = delete;
    VulkanCommandContext& operator=(const VulkanCommandContext&) = delete;

    VkResult AcquireNextImage(VkSwapchainKHR swapchain, uint32_t& imageIndex);
    void RecordCommandBuffer(uint32_t imageIndex, const std::function<void(VkCommandBuffer)>& recorder);
    void Submit(VkQueue graphicsQueue, uint32_t imageIndex);
    VkResult Present(VkQueue presentQueue, VkSwapchainKHR swapchain, uint32_t imageIndex);
    void WaitForAllFrames();
    // Frames counted by Submit, 1 for the first. A resource the frames submitted so far may use is
    // free once CompletedSubmits() reaches LastSubmit() as it was then (VulkanRetireQueue).
    uint64_t LastSubmit() const;
    // The last submit known to have finished: polls the frame slots' fences.
    uint64_t CompletedSubmits();

    // The slot the frame being recorded belongs to. Advances in Present, so it is stable for the
    // whole of one AcquireNextImage / Submit / Present cycle.
    uint32_t GetCurrentFrame() const;

  private:
    void CreateCommandPool(const QueueFamilyIndices& queueFamilies);
    void AllocateCommandBuffers(size_t commandBufferCount);
    void CreateSyncObjects(size_t swapchainImageCount);

    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VulkanFrameSyncObjects> m_frameSyncObjects;
    // Signaled by Submit and waited on by Present. Indexed by swapchain image (not by frame in
    // flight): the presentation engine may still be waiting on the semaphore after the frame's
    // fence has signaled, so a per-frame semaphore could be reused while still in use. Reuse per
    // image is safe because reacquiring an image implies its previous present consumed the wait.
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_imagesInFlight;
    uint32_t m_currentFrame = 0;
    uint64_t m_lastSubmit = 0;
    uint64_t m_completedSubmits = 0;
    // The submit each frame slot's fence was last armed for.
    std::vector<uint64_t> m_slotSubmits;
};
}
