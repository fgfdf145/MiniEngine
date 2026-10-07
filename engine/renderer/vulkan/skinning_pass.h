#pragma once

#include "common.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace me
{

class VulkanBuffer;

// Linear blend skinning on the GPU (skin.comp): each skinned submesh's bind pose vertices, deformed by
// its entity's joint palette, written over its vertex and position buffers (VulkanBuffer::IsSkinned)
// at the start of the frame, before anything draws them. Every pass then draws the posed mesh as it
// draws any other: the shadow maps, the G-buffer, the forward and toon passes and the selection.
//
// Not posed: the ray scene's hierarchies, built from the CPU's bind pose, and the motion vectors,
// which carry the entity's motion alone.
class VulkanSkinningPass
{
  public:
    // At most this many joint matrices a frame, every skinned submesh's palette together.
    static constexpr uint32_t kMaxPaletteMatrices = 32768;
    // At most this many skinned submeshes alive at once.
    static constexpr uint32_t kMaxSkinnedMeshes = 2048;

    struct Dispatch
    {
        const VulkanBuffer* buffer = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        // The submesh's joints: palette[paletteOffset, paletteOffset + jointCount).
        const std::vector<glm::mat4>* palette = nullptr;
        uint32_t paletteOffset = 0;
        uint32_t jointCount = 0;
        // A tyre (MeshData::deformable): tyre_deform.comp instead of skin.comp, its "joints" the two
        // matrices of its TyreDeformation (PackTyreDeformation).
        bool tyre = false;
    };

    VulkanSkinningPass(VkPhysicalDevice physicalDevice, VkDevice device, VkPipelineCache pipelineCache, uint32_t frameSlotCount);
    ~VulkanSkinningPass();

    VulkanSkinningPass(const VulkanSkinningPass&) = delete;
    VulkanSkinningPass& operator=(const VulkanSkinningPass&) = delete;

    // The set naming a posed buffer's five buffers (a tyre's has no skin: its bind pose stands in);
    // Release it when the buffer goes. It is freed a
    // few frames later, once no frame in flight can still be dispatching with it.
    VkDescriptorSet Acquire(const VulkanBuffer& buffer);
    void Release(VkDescriptorSet set);

    // Copies the palettes into the frame slot's buffer and records every dispatch between the
    // barriers that order it after last frame's draws and before this frame's.
    void Record(VkCommandBuffer commandBuffer, uint32_t frameSlot, std::span<const Dispatch> dispatches);

  private:
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_meshSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_paletteSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_tyrePipeline = VK_NULL_HANDLE;
    std::vector<VkBuffer> m_paletteBuffers;
    std::vector<VkDeviceMemory> m_paletteMemory;
    std::vector<void*> m_paletteMapped;
    std::vector<VkDescriptorSet> m_paletteSets;
    // Released sets and the frame they were released in; Record frees the ones old enough.
    struct PendingFree
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t frame = 0;
    };
    std::vector<PendingFree> m_pendingFrees;
    uint64_t m_frame = 0;
};
}
