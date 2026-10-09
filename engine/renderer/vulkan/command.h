#pragma once

#include "common.h"
#include <engine/renderer/material_pipeline.h>
#include <engine/scene/toon_material.h>

#include <entt/entt.hpp>

#include <array>
#include <functional>

#include <nvrhi/nvrhi.h>

namespace me
{

struct VulkanDrawItem
{
    nvrhi::IBuffer* vertexBuffer = nullptr;
    nvrhi::IBuffer* indexBuffer = nullptr;
    uint32_t indexCount = 0;
    // Set 1, the material's textures (VulkanMaterialSetCache).
    nvrhi::IBindingSet* materialSet = nullptr;
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
    // Where each vertex was last frame (VulkanBuffer::GetPreviousPositionBuffer): binding 1 of the
    // material pipelines, whose motion vectors start from it.
    nvrhi::IBuffer* previousPositionBuffer = nullptr;
    // The material's samplers as indices into set 0's table (GpuMaterialData::samplerIndices), which
    // the toon passes copy into their own materials.
    std::array<uint32_t, 8> samplerIndices{};
};

class NvrhiDevice;

// The frame's command list and its submission: one NVRHI command list, opened each frame, whose
// Vulkan command buffer the passes still record into directly while they move to NVRHI
// (docs/design/2026-10-08-nvrhi-backend-design.md); the swapchain's acquire and present stay native.
class VulkanCommandContext
{
  public:
    // How many frames may be recorded before the oldest must complete. AcquireNextImage waits for
    // this slot's last frame before acquiring, so any resource indexed by GetCurrentFrame() is free
    // for reuse once that call returns. SceneRenderTargets sizes its transient targets against this.
    static constexpr size_t kMaxFramesInFlight = 2;

    VulkanCommandContext(NvrhiDevice& nvrhi, VkDevice device, size_t swapchainImageCount);
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
    // The last submit known to have finished: polls the frame slots' event queries.
    uint64_t CompletedSubmits();

    // The frame's NVRHI command list, open while RecordCommandBuffer's recorder runs: the recorder's
    // command buffer is its native one.
    nvrhi::ICommandList* GetCommandList() const;

    // The slot the frame being recorded belongs to. Advances in Present, so it is stable for the
    // whole of one AcquireNextImage / Submit / Present cycle.
    uint32_t GetCurrentFrame() const;

  private:
    NvrhiDevice& m_nvrhi;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::CommandListHandle m_commandList;
    // Per frame slot: the semaphore the swapchain signals when the acquired image is free, and the
    // query that completes with the slot's last submit.
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<nvrhi::EventQueryHandle> m_frameQueries;
    // Signaled by Submit and waited on by Present. Indexed by swapchain image (not by frame in
    // flight): the presentation engine may still be waiting on the semaphore after the frame has
    // finished, so a per-frame semaphore could be reused while still in use. Reuse per image is safe
    // because reacquiring an image implies its previous present consumed the wait.
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    // Per swapchain image: the frame slot whose submit last rendered to it, -1 for none.
    std::vector<int> m_imagesInFlight;
    uint32_t m_currentFrame = 0;
    uint64_t m_lastSubmit = 0;
    uint64_t m_completedSubmits = 0;
    // The submit each frame slot's query was last set for (0 for none yet).
    std::vector<uint64_t> m_slotSubmits;
};
}
