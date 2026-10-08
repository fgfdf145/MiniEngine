#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <array>
#include <cstdint>
#include <vector>

namespace me
{

// One bounce of diffuse light in screen space: the indirect half of the visibility bitmask the AO
// marches (gi_trace.comp). Three passes after lighting: the trace reads depth, the geometric normal
// and the lit HDR image and writes GiRaw; the resolve filters it into SceneGi; the composite adds
// SceneGi times each pixel's diffuse albedo to the HDR image. Only the deferred order has them, and
// the trace and composite record nothing while GI is off (the resolve then writes zero to SceneGi
// for the debug view).
class VulkanGiTracePass : public IScenePass
{
  public:
    VulkanGiTracePass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanGiTracePass() override;

    VulkanGiTracePass(const VulkanGiTracePass&) = delete;
    VulkanGiTracePass& operator=(const VulkanGiTracePass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateBindingSets(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};

// Spatial filter plus temporal accumulation into SceneGi, with its history outside the layout
// tracker as VulkanAoResolvePass keeps its own. The history is RGBA32F: rgb and, packed into alpha,
// the view distance and the sample count (gi_resolve.comp). The trace and the resolve record through
// NVRHI (NvrhiPassScope); GiRaw, SceneGi and the history images come and go in GENERAL.
class VulkanGiResolvePass : public IScenePass
{
  public:
    VulkanGiResolvePass(nvrhi::IDevice* nvrhiDevice, VkDevice device, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanGiResolvePass() override;

    VulkanGiResolvePass(const VulkanGiResolvePass&) = delete;
    VulkanGiResolvePass& operator=(const VulkanGiResolvePass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateBindingSets(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    HistoryImagePair m_history;
    // Indexed by copy * 2 + readIndex, as VulkanAoResolvePass's.
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
};

// A full-screen triangle over the HDR target, blended ONE + ONE, that adds SceneGi through each
// pixel's diffuse albedo (gi_composite.frag). It binds the lighting pass's sets: the camera at set
// 0, the empty set 1 and the G-buffer, SceneGi included, at set 2.
class VulkanGiCompositePass : public IScenePass
{
  public:
    VulkanGiCompositePass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout emptySetLayout,
        VkDescriptorSetLayout gbufferSetLayout);
    ~VulkanGiCompositePass() override;

    VulkanGiCompositePass(const VulkanGiCompositePass&) = delete;
    VulkanGiCompositePass& operator=(const VulkanGiCompositePass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
};
}
