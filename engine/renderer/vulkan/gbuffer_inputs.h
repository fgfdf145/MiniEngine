#pragma once

#include "common.h"
#include "render_target_layout.h"
#include "scene_render_targets.h"

#include <array>
#include <vector>

namespace me
{

// Set 2 of every pipeline that samples the G-buffer: GB0-GB3, depth, the motion vectors, the
// resolved AO and GB5 as combined image samplers, one set per frame slot because all eight are transient
// targets. The lighting pass and
// the tone mapping debug views both bind it, which is why the renderer owns it and not either
// pass.
//
// It also owns the empty layout those pipelines declare at set 1. Vulkan requires every set index
// below the highest one a pipeline layout uses to be declared, and both pipelines put the G-buffer
// at set 2 so shaders/vulkan/gbuffer_inputs.glsl can declare it once for both.
class VulkanGBufferDescriptors
{
  public:
    // Binding order: binding N samples kInputs[N]. gbuffer_inputs.glsl declares the same order.
    static constexpr std::array<RenderTargetId, 8> kInputs = {
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::SceneAo,
        RenderTargetId::GBufferCustom};

    VulkanGBufferDescriptors(VkDevice device, const SceneRenderTargets& targets);
    ~VulkanGBufferDescriptors();

    VulkanGBufferDescriptors(const VulkanGBufferDescriptors&) = delete;
    VulkanGBufferDescriptors& operator=(const VulkanGBufferDescriptors&) = delete;

    VkDescriptorSetLayout GetSetLayout() const;
    VkDescriptorSetLayout GetEmptySetLayout() const;

    // Routed through ResolveIndex like every other per-copy lookup, so the frame slot rule lives in
    // SceneRenderTargets alone.
    VkDescriptorSet GetSet(const SceneRenderTargets& targets, uint32_t imageIndex, uint32_t frameSlot) const;

    // Rewrites every set against the rebuilt views. Must run with in-flight frames waited on.
    void OnTargetsRebuilt(const SceneRenderTargets& targets);

  private:
    void CreateSetLayouts();
    void CreateSampler();
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    // Shared by the destructor and the constructor's unwind path, as in every pass.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_emptySetLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
