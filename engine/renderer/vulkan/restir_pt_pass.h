#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <array>
#include <cstdint>
#include <vector>

namespace me
{

class VulkanRayScene;

// Real-time path tracing with ReSTIR PT Enhanced (Lin, Kettunen and Wyman 2026; docs/design/
// 2026-10-07-restir-pt-enhanced-design.md). Between the geometry and lighting passes, five dispatches
// over the deferred pixels (shaders/vulkan/restir_pt_*.comp):
//   initial: each pixel's primary surface record, then a path tree from it with NEE at every vertex,
//     one path kept by weighted reservoir sampling, direct and indirect light in one reservoir;
//   temporal: resampling with last frame's reservoir, its confidence capped by the duplication map;
//   spatial shift and spatial: paired reuse with each pixel's partners in three pairing textures, which
//     shades the pixel (ScenePathTrace, pre-exposed) and keeps the final reservoir as history;
//   duplication: how many neighbours share each final reservoir's sample.
// The lighting pass adds ScenePathTrace in place of its lights while the frame context's pathTracing is
// on. Hardware ray tracing only; records nothing while off.
//
// The per-pixel buffers (about 260 bytes a pixel) are made by Prepare the first frame the path tracer
// runs, and remade at the new size when the targets are; never while it has not run.
class VulkanRestirPtPass : public IScenePass
{
  public:
    VulkanRestirPtPass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        const VulkanRayScene& rayScene);
    ~VulkanRestirPtPass() override;

    VulkanRestirPtPass(const VulkanRestirPtPass&) = delete;
    VulkanRestirPtPass& operator=(const VulkanRestirPtPass&) = delete;

    // The device has the ray query pipelines.
    bool IsAvailable() const;

    // Makes the buffers and descriptor sets for the current targets if they do not exist yet. True when
    // it made them: their contents are undefined, so the history must be reset. Render thread, before
    // recording.
    bool Prepare(const SceneRenderTargets& targets);

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        nvrhi::BufferHandle handle;
        VkDeviceSize size = 0;
    };

    Buffer CreateBuffer(VkDeviceSize size, bool hostVisible) const;
    void DestroyBuffer(Buffer& buffer) const;
    void CreateResources(const SceneRenderTargets& targets);
    void DestroyResources();
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    // Set 2 of every pipeline: 0-5 the G-buffer (depth, normal, albedo, surface, coat, velocity),
    // 6 ScenePathTrace (storage), 7-14 the buffers (restir_pt_common.slang).
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Frame set, ray set, this pass's set, ray texture table.
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_initialPipeline = VK_NULL_HANDLE;
    VkPipeline m_temporalPipeline = VK_NULL_HANDLE;
    VkPipeline m_spatialShiftPipeline = VK_NULL_HANDLE;
    VkPipeline m_spatialPipeline = VK_NULL_HANDLE;
    VkPipeline m_duplicationPipeline = VK_NULL_HANDLE;

    bool m_prepared = false;
    VkExtent2D m_extent{};
    Buffer m_workReservoirs;
    Buffer m_historyReservoirs;
    // Indexed by TemporalHistoryFrame: the write index holds this frame's surfaces, the read index last
    // frame's.
    std::array<Buffer, 2> m_surfaces;
    Buffer m_shifts;
    Buffer m_duplication;
    Buffer m_pairing;
    Buffer m_accumulation;
    // Indexed by transient copy * 2 + the surfaces' write index.
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
