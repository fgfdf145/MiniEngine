#pragma once

#include "nvrhi_pass.h"
#include "scene_pass.h"

#include <vector>

namespace me
{

// Which half of the forward drawing a VulkanForwardPass records. The transmission copy runs
// between them, so transmissive surfaces see everything the first half drew.
enum class ForwardPassPart
{
    // The Opaque and Mask items it shades (all of them when it owns the frame, the forward-shaded
    // ones otherwise), then the sky into the pixels no geometry covered.
    OpaqueAndSky,
    // Transmissive items back to front, then Blend items back to front, over what is there.
    Translucent
};

// The material pass into the HDR target with depth. In the forward-only comparison order its
// opaque half owns the frame and draws every opaque item; in the deferred order it draws only the
// forward-shaded ones over the lighting pass's result. Both attachments declare initialLayout ==
// finalLayout so the pass performs no implicit transition.
//
// Framebuffers are indexed by frame slot because both attachments are transient targets.
class VulkanForwardPass : public IScenePass
{
  public:
    VulkanForwardPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout, ForwardPassPart part);
    ~VulkanForwardPass() override;

    VulkanForwardPass(const VulkanForwardPass&) = delete;
    VulkanForwardPass& operator=(const VulkanForwardPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The material pipelines are built against this: HDR colour and depth, which a resize never
    // changes.
    const nvrhi::FramebufferInfo& GetFramebufferInfo() const;

  private:
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void RecordSky(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const ScenePassFrameContext& frame) const;

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    ForwardPassPart m_part = ForwardPassPart::OpaqueAndSky;
    // One per transient copy of the targets.
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
    // sky.vert and sky.frag: set 0 plus a 16-byte push constant (register space 1), the background
    // radiance.
    PushConstantLayout m_skyConstants;
    nvrhi::GraphicsPipelineHandle m_skyPipeline;
};
}
