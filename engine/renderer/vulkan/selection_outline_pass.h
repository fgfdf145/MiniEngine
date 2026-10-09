#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"
#include "shadow_pass.h"

#include <memory>

#include <vector>

namespace me
{

// The editor's selection outline, as Blender draws it: a line of the active object's orange around
// the selected entity's silhouette, full strength where the entity is in view and faint where
// something nearer hides it.
//
// Two passes. The mask pass draws the selected entity's submeshes (ScenePassFrameContext::
// selectionDrawItems) into SelectionDepth alone, unjittered, with the shadow casters' shaders: depth
// only, and the alpha test for Mask materials. The outline pass then finds, for every pixel outside
// that silhouette, the distance to the nearest pixel inside it, and turns that into the line's
// coverage; comparing the entity's depth with the scene's there tells whether that part is hidden.
// It writes SelectionOutline, transparent but for the line, which ImGui draws over the viewport
// image: the outline never enters the tone mapped image, so captures and recordings leave it out.

class VulkanSelectionMaskPass : public IScenePass
{
  public:
    VulkanSelectionMaskPass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout);
    ~VulkanSelectionMaskPass() override;

    VulkanSelectionMaskPass(const VulkanSelectionMaskPass&) = delete;
    VulkanSelectionMaskPass& operator=(const VulkanSelectionMaskPass&) = delete;

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
    std::unique_ptr<ShadowCasterRenderer> m_casters;
    // One per SelectionDepth copy, indexed by frame slot.
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};

class VulkanSelectionOutlinePass : public IScenePass
{
  public:
    VulkanSelectionOutlinePass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets);
    ~VulkanSelectionOutlinePass() override;

    VulkanSelectionOutlinePass(const VulkanSelectionOutlinePass&) = delete;
    VulkanSelectionOutlinePass& operator=(const VulkanSelectionOutlinePass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateBindingSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::GraphicsPipelineHandle m_pipeline;
    // The two depths it reads are transient, so the sets are indexed by frame slot; the outline is
    // ImGui's, so the framebuffers are indexed by swapchain image.
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};
}
