#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"
#include "uniform_buffer.h"

namespace me
{

// The path traced layer of the forward-shaded surfaces (docs/design/2026-10-08-path-tracing-missing-
// effects-design.md): glass and the other Blend surfaces, and the Opaque and Mask ones the forward
// pass shades (transmission, iridescence), are not in the G-buffer the path tracer starts from. In
// path tracing mode this pass draws them twice with gbuffer.frag, before the path tracer:
//   depth (kLayerPass 1): each pixel's nearest such surface in front of the opaque scene, its depth
//     kept by a MAX blend into an R32F image (reverse-Z; 0 where there is none), against the scene's
//     depth (tested, not written);
//   surface (kLayerPass 2): the G-buffer of the fragment at that depth alone (albedo, normals,
//     metallic and roughness, motion), in the G-buffer's own formats, for VulkanPathTracePass to trace
//     and denoise as it does the opaque surfaces'.
// triangle.frag then takes the traced light where its fragment is that surface (set 0 bindings 29 to
// 31). A Blend surface covers a pixel when its alpha reaches PATH_TRACE_LAYER_MIN_ALPHA there.
//
// The images are made by the first frame that path traces (Prepare) and rest in
// SHADER_READ_ONLY_OPTIMAL, the layout set 0 names them in: the render passes take them from UNDEFINED
// (they are redrawn) and back. Needs a blendable R32_SFLOAT (IsSupported).
class VulkanPathTraceLayerPass : public IScenePass
{
  public:
    static constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
    // gbuffer.frag's outputs (VulkanGeometryPass::kAttachments' colours): the surface pass keeps
    // albedo, normal, surface and velocity, the rest unused.
    static constexpr uint32_t kSurfaceColorSlots = 8;

    VulkanPathTraceLayerPass(VkPhysicalDevice physicalDevice, VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets);
    ~VulkanPathTraceLayerPass() override;

    VulkanPathTraceLayerPass(const VulkanPathTraceLayerPass&) = delete;
    VulkanPathTraceLayerPass& operator=(const VulkanPathTraceLayerPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    bool IsSupported() const;
    // The two pipeline sets are built against these.
    VkRenderPass GetDepthRenderPass() const;
    VkRenderPass GetSurfaceRenderPass() const;

    // Makes the images at the targets' size; true when it made them (the caller then moves them to
    // their resting layout with RecordInitialTransition and points set 0 at them).
    bool Prepare(const SceneRenderTargets& targets);
    bool IsReady() const;
    void RecordInitialTransition(VkCommandBuffer commandBuffer) const;

    // Nearest, clamped. The depth for set 0 binding 29; all four for the path tracer's layer set.
    TextureDescriptorBinding GetDepthBinding() const;
    VkImageView GetDepthView() const;
    VkImageView GetAlbedoView() const;
    VkImageView GetNormalView() const;
    VkImageView GetSurfaceView() const;
    VkImageView GetVelocityView() const;

  private:
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        nvrhi::TextureHandle texture;
        VkImageView view = VK_NULL_HANDLE;
    };
    enum SurfaceImage : uint32_t
    {
        kAlbedo,
        kNormal,
        kSurface,
        kVelocity,
        kSurfaceImageCount
    };

    void CreateRenderPasses(const SceneRenderTargets& targets);
    void CreateImage(VkFormat format, VkExtent2D extent, Image& image) const;
    void DestroyImages();
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    bool m_supported = false;
    nvrhi::SamplerHandle m_sampler;
    std::array<VkFormat, kSurfaceImageCount> m_surfaceFormats{};
    VkRenderPass m_depthRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_surfaceRenderPass = VK_NULL_HANDLE;
    Image m_depth;
    std::array<Image, kSurfaceImageCount> m_surfaceImages{};
    // The depth pass's, one per copy of the scene's depth it tests against; the surface pass's.
    std::vector<VkFramebuffer> m_depthFramebuffers;
    VkFramebuffer m_surfaceFramebuffer = VK_NULL_HANDLE;
    bool m_ready = false;
    mutable bool m_initialized = false;
};
}
