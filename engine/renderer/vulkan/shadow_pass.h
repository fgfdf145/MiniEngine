#pragma once

#include "common.h"
#include "gpu_timer.h"
#include "nvrhi_native.h"
#include "nvrhi_pass.h"
#include "uniform_buffer.h"

#include <engine/renderer/material.h>
#include <engine/renderer/shadow_cascades.h>

#include <array>
#include <memory>
#include <optional>
#include <span>

namespace me
{

class VulkanParallelRecorder;

// What the shadow pass needs to draw one caster. Blend materials are not casters.
// What the alpha test reads of a caster's material: a few floats rather than the whole GpuMaterialData,
// since a map has tens of thousands of casters a frame.
struct ShadowAlphaTestMaterial
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float nodeGraphFactors[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    float alphaCutoff = 0.5f;
};

struct ShadowDrawItem
{
    nvrhi::IBuffer* vertexBuffer = nullptr;
    // The position-only stream, which opaque casters draw from.
    nvrhi::IBuffer* positionBuffer = nullptr;
    nvrhi::IBuffer* indexBuffer = nullptr;
    uint32_t indexCount = 0;
    glm::mat4 model{1.0f};
    // A world space sphere around the caster; each cascade skips casters that miss its volume.
    glm::vec3 worldBoundsCenter{0.0f};
    float worldBoundsRadius = 0.0f;
    // Mask materials run the alpha test and need their material set; opaque ones need neither.
    bool alphaMask = false;
    nvrhi::IBindingSet* materialSet = nullptr;
    // The draw slot whose material the alpha test reads from the frame set (materials, texture
    // transforms); the two below are what of it reaches the map, for the caster key.
    uint32_t drawSlot = 0;
    ShadowAlphaTestMaterial material;
    // The base colour's texture transform rows (GpuTextureTransforms slot 0).
    float baseColorTransform[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
};

// Push constants for shaders/vulkan/shadow.vert, shadow_depth.vert and shadow.frag (register space 2).
struct ShadowPushConstants
{
    glm::mat4 lightModelViewProjection{1.0f};
    uint32_t drawSlot = 0;
    uint32_t padding[3] = {};
};

static_assert(sizeof(ShadowPushConstants) == 80, "ShadowPushConstants must match the shadow shaders' block");

// Draws shadow casters (ShadowDrawItem) into a depth target: opaque ones from their position stream
// with no fragment shader, Mask ones with the alpha test (shadow.frag), which reads the caster's
// material from a frame set (set 0) and its textures from its material set (set 1). The cascades,
// the local shadow atlas and the selection's own depth all draw them so.
struct ShadowCasterPipelineDesc
{
    nvrhi::ComparisonFunc depthFunc = nvrhi::ComparisonFunc::Less;
    int depthBias = 0;
    float slopeScaledDepthBias = 0.0f;
};

class ShadowCasterRenderer
{
  public:
    ShadowCasterRenderer(
        nvrhi::IDevice* device,
        const nvrhi::FramebufferInfo& framebuffer,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout,
        const ShadowCasterPipelineDesc& desc);

    // Draws every caster whose bounds `visible` accepts with viewProjection, into framebuffer through
    // viewport. The caller has put the target in its depth write state.
    template <typename Visible>
    void Record(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        const nvrhi::ViewportState& viewport,
        nvrhi::IBindingSet* frameSet,
        const glm::mat4& viewProjection,
        std::span<const ShadowDrawItem> drawItems,
        Visible&& visible) const
    {
        for (const ShadowDrawItem& item : drawItems)
        {
            if (visible(item))
            {
                RecordDraw(commandList, framebuffer, viewport, frameSet, viewProjection, item);
            }
        }
    }

  private:
    void RecordDraw(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        const nvrhi::ViewportState& viewport,
        nvrhi::IBindingSet* frameSet,
        const glm::mat4& viewProjection,
        const ShadowDrawItem& item) const;

    PushConstantLayout m_constants;
    nvrhi::GraphicsPipelineHandle m_opaquePipeline;
    nvrhi::GraphicsPipelineHandle m_maskPipeline;
};

// Renders the cascaded shadow map of the directional light that casts shadows: a 2D array depth
// image with one layer per cascade, which the material pass samples through set 0, binding 1.
//
// This pass is not an IScenePass, and its image is not a SceneRenderTargets target. It has a fixed
// resolution rather than the viewport's, it has one layer per cascade, and it is one image shared
// by every frame in flight rather than a copy per frame. So it keeps its own states: the image rests
// as a shader resource, and each redrawn layer is a depth target for its clear and draws.
//
// The pass lives as long as the device: nothing in it depends on the swapchain or the viewport.
class VulkanShadowPass
{
  public:
    VulkanShadowPass(
        nvrhi::IDevice* nvrhiDevice,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout,
        uint32_t resolution);
    ~VulkanShadowPass();

    VulkanShadowPass(const VulkanShadowPass&) = delete;
    VulkanShadowPass& operator=(const VulkanShadowPass&) = delete;

    uint32_t GetResolution() const;
    // The array texture and comparison sampler the material pass binds.
    TextureDescriptorBinding GetSampledBinding() const;
    // Decides, before Record, which cascades this frame redraws (ShadowCascadeCache) and returns
    // what the map holds afterwards, which the shader must sample with. With no cascades (no light
    // casts shadows) it returns nothing and Record clears the layers once, the first such frame:
    // the material pass binds the map whether or not a light casts shadows. Layers keep their depth
    // across the frames that skip them.
    std::optional<ShadowCascadePlan> Plan(const ShadowCascades* cascades, uint64_t casterKey);
    // Renders the casters into the cascades Plan chose, with the plan it returned (null with no
    // cascades). frameSet: a frame set, for the alpha test's materials.
    void Record(
        nvrhi::ICommandList* commandList,
        nvrhi::IBindingSet* frameSet,
        std::span<const ShadowDrawItem> drawItems,
        const ShadowCascadePlan* plan,
        VulkanGpuTimer* timer = nullptr,
        VulkanParallelRecorder* recorder = nullptr) const;

    // Every cascade redraws on the next frame (a full restart, for repeatable captures).
    void InvalidateCache()
    {
        m_cache.Invalidate();
    }

  private:
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    uint32_t m_resolution = 0;
    nvrhi::TextureHandle m_texture;
    std::array<nvrhi::FramebufferHandle, kShadowCascadeCount> m_framebuffers{};
    nvrhi::SamplerHandle m_sampler;
    std::unique_ptr<ShadowCasterRenderer> m_casters;
    ShadowCascadeCache m_cache;
    // Which layers this frame's Record renders or clears, and whether they were last cleared for a
    // frame without a shadow light.
    std::array<bool, kShadowCascadeCount> m_frameRedraw{};
    bool m_cleared = false;
    // The image was moved out of its initial state once.
    mutable bool m_initialized = false;
};
}
