#pragma once

#include "common.h"
#include "nvrhi_native.h"

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

    struct Dispatch
    {
        const VulkanBuffer* buffer = nullptr;
        nvrhi::IBindingSet* set = nullptr;
        // The submesh's joints: palette[paletteOffset, paletteOffset + jointCount).
        const std::vector<glm::mat4>* palette = nullptr;
        uint32_t paletteOffset = 0;
        uint32_t jointCount = 0;
        // A tyre (MeshData::deformable): tyre_deform.comp instead of skin.comp, its "joints" the two
        // matrices of its TyreDeformation (PackTyreDeformation).
        bool tyre = false;
    };

    VulkanSkinningPass(nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount);
    ~VulkanSkinningPass();

    VulkanSkinningPass(const VulkanSkinningPass&) = delete;
    VulkanSkinningPass& operator=(const VulkanSkinningPass&) = delete;

    // The binding set naming a posed buffer's five buffers (a tyre's has no skin: its bind pose
    // stands in). Drop it when the buffer goes: NVRHI keeps it alive while a frame in flight still
    // dispatches with it.
    nvrhi::BindingSetHandle Acquire(const VulkanBuffer& buffer) const;

    // Copies the palettes into the frame slot's buffer and records every dispatch, the posed buffers
    // moved out of their read state for the writes and back after.
    void Record(nvrhi::ICommandList* commandList, uint32_t frameSlot, std::span<const Dispatch> dispatches);

  private:
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::BindingLayoutHandle m_meshSetLayout;
    nvrhi::BindingLayoutHandle m_paletteSetLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    nvrhi::ComputePipelineHandle m_tyrePipeline;
    std::vector<nvrhi::BufferHandle> m_paletteHandles;
    std::vector<void*> m_paletteMapped;
    std::vector<nvrhi::BindingSetHandle> m_paletteSets;
};
}
