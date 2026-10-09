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
// shaders/vulkan/toon_common.glsl).
class VulkanToonMaterials
{
  public:
    // At most this many toon draws a frame; the rest are left undrawn (with a warning).
    static constexpr uint32_t kMaxDraws = 512;

    VulkanToonMaterials(VkPhysicalDevice physicalDevice, VkDevice device, nvrhi::IDevice* nvrhiDevice, uint32_t frameSlotCount);
    ~VulkanToonMaterials();

    VulkanToonMaterials(const VulkanToonMaterials&) = delete;
    VulkanToonMaterials& operator=(const VulkanToonMaterials&) = delete;

    // Copies each draw's toon material to its index in the slot's buffer, its head frame moved by its
    // head pose (parallel to the draws: the joint's palette matrix, the identity for a rigid face);
    // returns how many fit.
    uint32_t Write(uint32_t frameSlot, std::span<const VulkanDrawItem> toonDrawItems, std::span<const glm::mat4> headPoses);
    VkDescriptorSetLayout GetSetLayout() const;
    VkDescriptorSet GetSet(uint32_t frameSlot) const;

  private:
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::vector<VkBuffer> m_buffers;
    std::vector<nvrhi::BufferHandle> m_handles;
    std::vector<void*> m_mapped;
    std::vector<VkDescriptorSet> m_sets;
};

// The push constants of every toon pipeline (DrawConstants in toon_common.glsl): the model matrix,
// the draw's index in VulkanToonMaterials and the frame's toon exposure scale.
struct ToonPushConstants
{
    glm::mat4 model{1.0f};
    uint32_t toonIndex = 0;
    float exposureScale = 1.0f;
    uint32_t padding[2] = {};
};
static_assert(sizeof(ToonPushConstants) == 80, "ToonPushConstants must match toon_common.glsl's block");

class VulkanToonPrepass : public IScenePass
{
  public:
    VulkanToonPrepass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout materialSetLayout,
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
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    const VulkanToonMaterials& m_materials;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    // Indexed by ToonPipelineIndex: opaque (writes the mask) or transparent (does not), each culling
    // back faces or none.
    std::array<VkPipeline, 4> m_pipelines{};
    std::vector<VkFramebuffer> m_framebuffers;
};

class VulkanToonPass : public IScenePass
{
  public:
    VulkanToonPass(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout materialSetLayout,
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
    void CreateRenderPass(const SceneRenderTargets& targets);
    void CreateTargetSetLayout();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout, VkDescriptorSetLayout materialSetLayout);
    void CreateTargetSets(const SceneRenderTargets& targets);
    void CreateFramebuffers(const SceneRenderTargets& targets);
    void DestroyFramebuffers();
    void DestroyHandles();

    VkDevice m_device = VK_NULL_HANDLE;
    const VulkanToonMaterials& m_materials;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    // Set 3: the prepass's linear depth and eye mask, per frame slot.
    VkDescriptorSetLayout m_targetSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_targetPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_targetSets;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    // Indexed by ToonPipelineIndex for the surfaces (opaque or transparent, culling back faces or
    // none), then the outline.
    std::array<VkPipeline, 5> m_pipelines{};
    std::vector<VkFramebuffer> m_framebuffers;
};
}
