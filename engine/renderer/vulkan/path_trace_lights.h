#pragma once

#include "common.h"
#include "nvrhi_native.h"

#include <cstdint>
#include <vector>

namespace me
{

class VulkanRayScene;

// The lights as the path tracer's next event estimation picks them, beyond the scene light list
// (docs/design/2026-10-08-path-tracing-remaining-work-design.md), both built on the GPU every frame:
//   - the emissive triangles (shaders/vulkan/emissive_lights_common.slang): every triangle of the ray
//     scene's installed submeshes whose material emits (VulkanRayScene::GetEmissiveSubmeshes), put in
//     world space with a four-way tree of their powers to pick one from. Per frame slot: the list of
//     (draw slot, triangle) and each slot's first light, written from the CPU when the ray scene's
//     emissive submeshes change; and what the GPU builds from them;
//   - the local lights' world grid (light_grid_common.slang): around the camera, each cell's local
//     lights, from which the vertices draw their candidates.
// Each frame slot's own sets bind them: the build's (set 2, written) and the trace's (set 4, read).
class VulkanPathTraceLights
{
  public:
    VulkanPathTraceLights(nvrhi::IDevice* nvrhiDevice, uint32_t frameCount, nvrhi::IBindingLayout* frameSetLayout, const VulkanRayScene& rayScene);
    ~VulkanPathTraceLights();

    VulkanPathTraceLights(const VulkanPathTraceLights&) = delete;
    VulkanPathTraceLights& operator=(const VulkanPathTraceLights&) = delete;

    // The trace's set 4, the buffers it reads: binding 0 triangles, 1 the power tree, 2 each slot's
    // first light, 3 each slot's instance, 4 the lights' (slot, triangle), 5 the light grid, all
    // structured buffers.
    nvrhi::IBindingLayout* GetTraceLayout() const;
    nvrhi::IBindingSet* GetTraceSet(uint32_t frameSlot) const;
    // The lights the frame slot's last Record built (0: none, and the trace must not read the set's
    // tree).
    uint32_t GetLightCount(uint32_t frameSlot) const;

    // Whether the frame slot's last Record built the light grid.
    bool HasLightGrid(uint32_t frameSlot) const;

    // Records the frame's builds, after the ray scene's own Record; afterwards the buffers are where the
    // trace reads them. emissive: brings the frame slot's list up to the ray scene's emissive submeshes
    // (its last frame has finished) and builds it, unless the scene emits nothing. lightGrid: builds the
    // grid over localLightCount local lights (none: nothing to build), frameIndex reseeding its choices.
    void Record(nvrhi::ICommandList* commandList, nvrhi::IBindingSet* frameSet, nvrhi::IBindingSet* raySet, nvrhi::IDescriptorTable* rayTextureTable,
                uint32_t frameSlot, uint32_t frameIndex, bool emissive, bool lightGrid, uint32_t localLightCount);

    // The most lights the list holds (4^10): the triangles past it are left out.
    static constexpr uint32_t kMaxLights = 1u << 20;

  private:
    struct Buffer
    {
        nvrhi::BufferHandle handle;
        void* mapped = nullptr;
        uint64_t size = 0;
    };
    struct Slot
    {
        // Device local, written by the build.
        Buffer triangles;
        Buffer tree;
        Buffer slotInstances;
        // Host visible, written when the ray scene's emissive submeshes change.
        Buffer slotBases;
        Buffer entries;
        // Device local, made on the first frame that builds the grid (a placeholder before).
        Buffer grid;
        bool gridMade = false;
        // What the last Record built.
        bool gridBuilt = false;
        bool emissiveBuilt = false;
        // The build's set 2 and the trace's set 4 over the slot's buffers.
        nvrhi::BindingSetHandle buildSet;
        nvrhi::BindingSetHandle traceSet;
        uint64_t generation = 0;
        uint32_t lightCount = 0;
        uint32_t depth = 1;
        bool valid = false;
    };

    // stride: the shaders' element size; hostVisible: the CPU writes it (mapped), else the build does.
    Buffer CreateBuffer(uint64_t size, uint32_t stride, bool hostVisible, const char* name) const;
    // Grows a buffer to hold size bytes (contents lost); true when it was remade.
    bool EnsureBuffer(Buffer& buffer, uint64_t size, uint32_t stride, bool hostVisible, const char* name) const;
    void UpdateSlot(Slot& slot);
    void CreateSets(Slot& slot) const;

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    const VulkanRayScene& m_rayScene;
    nvrhi::BindingLayoutHandle m_buildLayout;
    nvrhi::BindingLayoutHandle m_traceLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    nvrhi::ComputePipelineHandle m_gridPipeline;
    std::vector<Slot> m_slots;
};
}
