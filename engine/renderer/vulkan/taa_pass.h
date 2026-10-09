#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"

#include <vector>

namespace me
{

// Temporal anti-aliasing resolve (shaders/vulkan/taa_resolve.comp). Reads SceneHdr, depth and the
// motion vectors, and blends them with last frame's result into SceneTaa, which the exposure
// histogram and tone mapping read. Its two history images live here, outside the layout tracker,
// like the AO resolve's; which is read and whether it is valid arrive in the frame context. With TAA
// off, and in the forward-only order, it copies SceneHdr to SceneTaa unchanged.
//
// When the frame context carries DLSS, DLSS resolves instead, from the render size to the output
// size: this pass turns the G-buffer's motion vectors into the ones DLSS reads
// (dlss_motion_vectors.comp), evaluates DLSS into SceneTaa, and copies the result into the history
// image the SSR trace reads next frame, as the TAA resolve writes it.
//
// The resolve and the two DLSS input passes record through NVRHI (NvrhiPassScope); the history images
// and SceneTaa come and go in GENERAL, as the native passes hold them. NGX's evaluation and the copy
// after it stay native.
class VulkanTaaPass : public IScenePass
{
  public:
    VulkanTaaPass(nvrhi::IDevice* nvrhiDevice, VkDevice device, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout);
    ~VulkanTaaPass() override;

    VulkanTaaPass(const VulkanTaaPass&) = delete;
    VulkanTaaPass& operator=(const VulkanTaaPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // One of the two history images (last frame's anti-aliased result is at the frame's
    // taaHistory.readIndex), for the SSR trace, which takes its reflected colour from it. The
    // images are recreated in OnTargetsRebuilt, so a reader must rebuild its descriptors after
    // this pass has.
    VkImageView GetHistoryView(uint32_t index) const;

  private:
    // An image DLSS reads, written here at the render size: NVRHI's, kept in ShaderResource (the
    // layout NGX reads it in) between frames, with the native handles NGX takes.
    struct DlssInputImage
    {
        nvrhi::TextureHandle texture;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    DlssInputImage CreateDlssInputImage(VkExtent2D extent, VkFormat format, const char* name) const;
    void CreateBindingSets(const SceneRenderTargets& targets);
    void RecordDlss(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const;

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::SamplerHandle m_linearSampler;
    nvrhi::BindingLayoutHandle m_setLayout;
    nvrhi::ComputePipelineHandle m_pipeline;
    HistoryImagePair m_history;
    // Indexed by copy * 2 + readIndex: set r samples history r and stores to history 1 - r.
    std::vector<nvrhi::BindingSetHandle> m_bindingSets;

    // DLSS's motion vectors (RG16F, render size) and the pass that writes them, one set per copy.
    nvrhi::BindingLayoutHandle m_motionSetLayout;
    nvrhi::ComputePipelineHandle m_motionPipeline;
    std::vector<nvrhi::BindingSetHandle> m_motionBindingSets;
    DlssInputImage m_motion;
    // Ray reconstruction's guides (dlss_rr_guides.comp): diffuse albedo, specular albedo, normal and
    // roughness, the specular hit distance and the reflections' motion vectors, written before a ray
    // reconstruction evaluation; one set per transient copy names the G-buffer they are made from.
    std::array<DlssInputImage, 5> m_guides{};
    nvrhi::BindingLayoutHandle m_guideSetLayout;
    nvrhi::ComputePipelineHandle m_guidePipeline;
    std::vector<nvrhi::BindingSetHandle> m_guideBindingSets;
};
}
