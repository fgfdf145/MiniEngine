#pragma once

#include "scene_pass.h"

#include <vector>

namespace me
{

class VulkanRayScene;

// Resolves the G-buffer into HDR radiance with one full-screen triangle: decodes GB0-GB3,
// reconstructs world position from depth, and runs pbr_common.slang's ShadeSurface, the arithmetic
// triangle.frag also runs, directional shadow included. Pixels no geometry wrote resolve to the
// background radiance, pushed as a constant.
//
// Pipeline layout: set 0 the camera and shadow map (the frame set layout the material pipelines
// use), set 1 the empty filler, set 2 the G-buffer inputs, and a 16-byte fragment push constant;
// see VulkanGBufferDescriptors for why set 1 is empty. The shadow map is not a render target and
// is absent from Io(): VulkanShadowPass orders itself against every fragment shader read.
// Framebuffers are indexed by frame slot because the HDR target is transient.
//
// With hardware ray tracing a second pipeline, the shader's ray query variant, traces the local
// lights' shadows instead of reading the shadow atlas (RayTracingSettings::localShadows); its set 1,
// which the plain one leaves empty, is the ray scene and its set 3 the ray texture table.
class VulkanLightingPass : public IScenePass
{
  public:
    VulkanLightingPass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* gbufferSetLayout,
        const VulkanRayScene& rayScene);
    ~VulkanLightingPass() override;

    VulkanLightingPass(const VulkanLightingPass&) = delete;
    VulkanLightingPass& operator=(const VulkanLightingPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateFramebuffers(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    // The push constants' own layout (register space 4) and its binding set.
    nvrhi::BindingLayoutHandle m_pushLayout;
    nvrhi::BindingSetHandle m_pushSet;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    nvrhi::GraphicsPipelineHandle m_tracedPipeline;
    // One per transient copy of SceneHdr.
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};
}
