#pragma once

#include "nvrhi_native.h"
#include "scene_pass.h"

#include <array>
#include <vector>

namespace me
{

// The anime character shading of AnimateApp's "Universal Render Pipeline/Anime/Character" (a
// NiloToon clone; docs/design/2026-10-07-yuki-toon-shading-design.md), for the draws whose material
// is a toon one (VulkanDrawItem::toon, ScenePassFrameContext::toonDrawItems).
//
// The opaque ones are in the G-buffer like any deferred draw, which gives them depth, normals and
// motion vectors; their forward flag keeps the lighting pass off their pixels. Then two passes:
//
// - The prepass draws every toon draw into the characters' own depth (ToonDepth) and writes their
//   linear view depth (ToonLinearDepth), the depth texture the rim light and contact shadow read,
//   the face pushed back as Unity's depth-only pass pushes it, and the eye mask (ToonMask): 1 where
//   a material that writes Unity's stencil (the eyes) is the nearest opaque character surface. The
//   transparent ones (front hair and its redraw, eye shadow) add their depth but not to the mask.
// - The toon pass shades over the HDR target and the scene depth: the opaque draws in the render
//   queue's order, the outlines (inverted hulls pushed out along the smoothed normals), then the
//   transparent draws in queue order, blended, the front hair kept off the eye mask and its redraw
//   kept to it, which is the stencil test the shader makes.

// The toon materials of a frame's toon draws, in the order of ScenePassFrameContext::toonDrawItems,
// in a host-visible storage buffer per frame slot (set 2 of the toon pipelines; ToonMaterialBuffer in
// shaders/vulkan/toon_common.slang).
class VulkanToonMaterials
{
  public:
    // At most this many toon draws a frame; the rest are left undrawn (with a warning).
    static constexpr uint32_t kMaxDraws = 512;

    VulkanToonMaterials(nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount);
    ~VulkanToonMaterials();

    VulkanToonMaterials(const VulkanToonMaterials&) = delete;
    VulkanToonMaterials& operator=(const VulkanToonMaterials&) = delete;

    // Copies each draw's toon material to its index in the slot's buffer, its head frame moved by its
    // head pose (parallel to the draws: the joint's palette matrix, the identity for a rigid face);
    // returns how many fit.
    uint32_t Write(uint32_t frameSlot, std::span<const VulkanDrawItem> toonDrawItems, std::span<const glm::mat4> headPoses);

    // Set 2 of the toon pipelines: the materials and the push constants (register space 2).
    nvrhi::IBindingLayout* GetSetLayout() const;
    nvrhi::IBindingSet* GetSet(uint32_t frameSlot) const;

  private:
    nvrhi::BindingLayoutHandle m_setLayout;
    std::vector<nvrhi::BufferHandle> m_buffers;
    std::vector<void*> m_mapped;
    std::vector<nvrhi::BindingSetHandle> m_sets;
};

// The push constants of every toon pipeline (DrawConstants in toon_common.slang): the model matrix,
// the draw's index in VulkanToonMaterials and the frame's toon exposure scale.
struct ToonPushConstants
{
    glm::mat4 model{1.0f};
    uint32_t toonIndex = 0;
    float exposureScale = 1.0f;
    uint32_t padding[2] = {};
};
static_assert(sizeof(ToonPushConstants) == 80, "ToonPushConstants must match toon_common.slang's block");

class VulkanToonPrepass : public IScenePass
{
  public:
    VulkanToonPrepass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout,
        const VulkanToonMaterials& materials);
    ~VulkanToonPrepass() override;

    VulkanToonPrepass(const VulkanToonPrepass&) = delete;
    VulkanToonPrepass& operator=(const VulkanToonPrepass&) = delete;

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
    const VulkanToonMaterials& m_materials;
    // Indexed by ToonPipelineIndex: opaque (writes the mask) or transparent (does not), each culling
    // back faces or none.
    std::array<nvrhi::GraphicsPipelineHandle, 4> m_pipelines{};
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};

class VulkanToonPass : public IScenePass
{
  public:
    VulkanToonPass(
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
        nvrhi::IBindingLayout* materialSetLayout,
        const VulkanToonMaterials& materials);
    ~VulkanToonPass() override;

    VulkanToonPass(const VulkanToonPass&) = delete;
    VulkanToonPass& operator=(const VulkanToonPass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

  private:
    void CreateTargetSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);

    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    const VulkanToonMaterials& m_materials;
    // Set 3: the prepass's linear depth and eye mask, per frame slot.
    nvrhi::BindingLayoutHandle m_targetSetLayout;
    std::vector<nvrhi::BindingSetHandle> m_targetSets;
    // Indexed by ToonPipelineIndex for the surfaces (opaque or transparent, culling back faces or
    // none), then the outline.
    std::array<nvrhi::GraphicsPipelineHandle, 5> m_pipelines{};
    std::vector<nvrhi::FramebufferHandle> m_framebuffers;
};
}
