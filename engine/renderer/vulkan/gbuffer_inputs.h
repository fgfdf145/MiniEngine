#pragma once

#include "common.h"
#include "nvrhi_native.h"
#include "render_target_layout.h"
#include "scene_render_targets.h"

#include <array>
#include <vector>

namespace me
{

// Set 2 of every pipeline that samples the G-buffer: GB0-GB3, depth, the motion vectors, the
// resolved AO and the rest of kInputs, and the one sampler they are read with, one set per frame slot
// because all of them are transient targets. The layout and the sets are NVRHI's
// (docs/design/2026-10-08-nvrhi-backend-design.md, stage B3); the native pipelines take their
// Vulkan handles. The lighting pass and
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
    static constexpr std::array<RenderTargetId, 14> kInputs = {
        RenderTargetId::GBufferAlbedo,
        RenderTargetId::GBufferNormal,
        RenderTargetId::GBufferSurface,
        RenderTargetId::GBufferEmissive,
        RenderTargetId::SceneDepth,
        RenderTargetId::GBufferVelocity,
        RenderTargetId::SceneAo,
        RenderTargetId::GBufferSpecular,
        RenderTargetId::GBufferCoat,
        RenderTargetId::GBufferSheen,
        RenderTargetId::SceneReflections,
        // Written after the lighting pass: only the GI composite and the debug view sample it.
        RenderTargetId::SceneGi,
        // The ray traced sun shadow, which the lighting pass reads in place of the cascades.
        RenderTargetId::SceneShadow,
        // ReSTIR PT's shading, which the lighting pass adds in place of its lights while it runs.
        RenderTargetId::ScenePathTrace};

    // The sampler's binding: one for every input, past them (shaders/vulkan/gbuffer_inputs.slang).
    static constexpr uint32_t kSamplerBinding = 64;

    VulkanGBufferDescriptors(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets);
    ~VulkanGBufferDescriptors();

    VulkanGBufferDescriptors(const VulkanGBufferDescriptors&) = delete;
    VulkanGBufferDescriptors& operator=(const VulkanGBufferDescriptors&) = delete;

    VkDescriptorSetLayout GetSetLayout() const;
    VkDescriptorSetLayout GetEmptySetLayout() const;

    // Routed through ResolveIndex like every other per-copy lookup, so the frame slot rule lives in
    // SceneRenderTargets alone.
    VkDescriptorSet GetSet(const SceneRenderTargets& targets, uint32_t imageIndex, uint32_t frameSlot) const;
    // The same layout and sets as NVRHI's, for the passes on NVRHI pipelines.
    nvrhi::IBindingLayout* GetBindingLayout() const;
    nvrhi::IBindingSet* GetBindingSet(const SceneRenderTargets& targets, uint32_t imageIndex, uint32_t frameSlot) const;

    // Rewrites every set against the rebuilt views. Must run with in-flight frames waited on.
    void OnTargetsRebuilt(const SceneRenderTargets& targets);

  private:
    void CreateSetLayouts();
    void CreateSampler(nvrhi::IDevice* nvrhiDevice);
    void CreateDescriptorSets(const SceneRenderTargets& targets);
    // Shared by the destructor and the constructor's unwind path, as in every pass.
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::BindingLayoutHandle m_setLayout;
    VkDescriptorSetLayout m_emptySetLayout = VK_NULL_HANDLE;
    nvrhi::SamplerHandle m_sampler;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<VkDescriptorSet> m_descriptorSets;
};
}
