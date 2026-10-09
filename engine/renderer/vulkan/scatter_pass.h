#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"
#include "uniform_buffer.h"

namespace me
{

// The scatter pre-pass of KHR_materials_volume_scatter, as the Khronos glTF Sample Viewer draws it
// (scatter.frag): the materials that scatter, alone, into their own RGBA16F colour and D32 depth at
// the scene's extent. triangle.frag (under kScatterPrepass) writes the diffuse light entering the
// surface, pre-exposed, with the draw slot + 1 in alpha; the forward pass then diffuses it through
// the screen (volume_scatter_common.slang), sampling both images through set 0 bindings 19 and 20.
//
// The images live here rather than in SceneRenderTargets: the frame set binds one image, not one per
// frame slot, and the forward pipelines read them outside the layout tracker. They rest in
// SHADER_READ_ONLY_OPTIMAL; the render pass takes them from UNDEFINED (their contents are redrawn)
// and back, its two external dependencies ordering it after the last frame's reads and before this
// frame's. On a frame without scatter items the pass draws nothing and the images keep what they
// held, which nothing samples. A resize recreates them; the renderer then points set 0 at the new
// ones (VulkanUniformBuffer::SetScatterImages).
class VulkanScatterPass : public IScenePass
{
  public:
    VulkanScatterPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets);
    ~VulkanScatterPass() override;

    VulkanScatterPass(const VulkanScatterPass&) = delete;
    VulkanScatterPass& operator=(const VulkanScatterPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // The scatter pipelines are built against this: RGBA16F light and D32 depth.
    const nvrhi::FramebufferInfo& GetFramebufferInfo() const;
    // What set 0 bindings 19 and 20 sample: nearest, clamped, resting as shader resources.
    TextureDescriptorBinding GetLightBinding() const;
    TextureDescriptorBinding GetDepthBinding() const;

  private:
    void CreateImages(VkExtent2D extent);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_sampler;
    VkExtent2D m_extent{};
    nvrhi::TextureHandle m_light;
    nvrhi::TextureHandle m_depth;
    nvrhi::FramebufferHandle m_framebuffer;
    // The images were moved to their resting state since they were made.
    mutable bool m_initialized = false;
};
}
