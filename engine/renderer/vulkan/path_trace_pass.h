#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"
#include "uniform_buffer.h"

#include <cstdint>
#include <vector>

namespace me
{

class VulkanPathTraceLayerPass;
class VulkanRayScene;

// GPU path tracing (docs/design/2026-10-07-path-tracing-design.md): the indirect light at every opaque
// deferred pixel, path traced through the ray scene with hardware ray queries, which the lighting pass
// multiplies by the surface's lobe albedos in place of the ambient terms. Between the reflection
// resolve and the lighting pass, up to five dispatches:
//   trace (path_trace.comp): a diffuse and a specular path a pixel, demodulated;
//   temporal (path_trace_temporal.comp): the accumulation, reprojected through the motion vectors,
//     each history tap kept only on the same surface, up to the frame's history cap (which a still
//     image grows to the reference's length);
//   filter (path_trace_filter.comp): three edge-aware a-trous iterations that fade out as a still
//     image converges, the last into SceneGi (diffuse) and SceneReflections (specular), which path
//     tracing mode takes over from the GI and SSR resolves.
// With accumulation and denoising off, or while DLSS ray reconstruction denoises, the trace writes
// the two targets itself. The pass owns its intermediate images (raw paths, three history pairs) and
// makes them on the first path traced frame, so they cost nothing until path tracing is used.
//
// The same five dispatches then trace the forward-shaded surfaces' layer
// (docs/design/2026-10-08-path-tracing-missing-effects-design.md): from VulkanPathTraceLayerPass's
// G-buffer instead of the scene's, into histories and a result pair of its own, which the forward
// pass samples (set 0 bindings 30 and 31) and which rest in SHADER_READ_ONLY_OPTIMAL between frames.
// It runs with either path tracer, ReSTIR PT included, and always denoises itself: ray reconstruction
// never sees it. The raw pair is the two's shared scratch.
class VulkanPathTracePass : public IScenePass
{
  public:
    VulkanPathTracePass(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        const SceneRenderTargets& targets,
        VkDescriptorSetLayout frameSetLayout,
        const VulkanRayScene& rayScene,
        TextureDescriptorBinding multiScattering);
    ~VulkanPathTracePass() override;

    VulkanPathTracePass(const VulkanPathTracePass&) = delete;
    VulkanPathTracePass& operator=(const VulkanPathTracePass&) = delete;

    ScenePassId Id() const override;
    RenderPassIo Io() const override;
    void Record(
        VkCommandBuffer commandBuffer,
        const SceneRenderTargets& targets,
        const ScenePassFrameContext& frame) const override;
    void OnTargetsRebuilt(const SceneRenderTargets& targets) override;

    // Whether the device runs the trace at all (hardware ray tracing).
    bool IsSupported() const;
    // Makes the intermediate images at the targets' size when a frame is about to path trace and they
    // do not exist yet; true when it made them, and their history must be reset. Recording before
    // this for a path traced frame records nothing.
    bool Prepare(const SceneRenderTargets& targets);
    // The same for the forward-shaded surfaces' layer, read from the layer pass's images (which must
    // exist); true when it made them: the caller resets the layer's history, moves the result pair to
    // its resting layout (RecordLayerInitialTransition) and points set 0 at it.
    bool PrepareLayer(const SceneRenderTargets& targets, const VulkanPathTraceLayerPass& layer);
    bool IsLayerReady() const;
    void RecordLayerInitialTransition(VkCommandBuffer commandBuffer) const;
    // The layer's traced light through the diffuse and the specular lobes, nearest, for set 0.
    TextureDescriptorBinding GetLayerDiffuseBinding() const;
    TextureDescriptorBinding GetLayerSpecularBinding() const;

  private:
    // The trace, and the temporal and filter dispatches the settings ask for, with one of the sets.
    void RecordPaths(
        VkCommandBuffer commandBuffer,
        const ScenePassFrameContext& frame,
        VkDescriptorSet passSet,
        bool accumulate,
        bool denoise,
        bool historyValid,
        float historyScale) const;
    void CreateRaw(VkExtent2D extent);
    void WriteDescriptorSets(const SceneRenderTargets& targets);
    void WriteLayerDescriptorSets(const VulkanPathTraceLayerPass& layer);
    void DestroyImages();
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_nearestSampler = VK_NULL_HANDLE;
    // The atmosphere's multiple-scattering LUT (binding 18), for the air along the paths.
    TextureDescriptorBinding m_multiScattering;
    // One set for all three shaders (path_trace_common.glsl's bindings 0-18).
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Frame set, ray set, this pass's set, ray texture table (the trace); frame set and this pass's set
    // (the other two), with the same push constants.
    VkPipelineLayout m_tracePipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_tracePipeline = VK_NULL_HANDLE;
    VkPipeline m_temporalPipeline = VK_NULL_HANDLE;
    VkPipeline m_filterPipeline = VK_NULL_HANDLE;
    // The raw paths (0 diffuse, 1 specular), rewritten every frame; and the accumulations of the two
    // channels and the surfaces they were made on, ping-ponged.
    HistoryImagePair m_raw;
    bool m_rawReady = false;
    HistoryImagePair m_diffuseHistory;
    HistoryImagePair m_specularHistory;
    HistoryImagePair m_surfaceHistory;
    bool m_imagesReady = false;
    // Indexed by transient copy * 2 + history read index; written once the images exist.
    std::vector<VkDescriptorSet> m_descriptorSets;
    // The layer's: its accumulations, its result (0 diffuse, 1 specular), its two sets (by history
    // read index).
    HistoryImagePair m_layerDiffuseHistory;
    HistoryImagePair m_layerSpecularHistory;
    HistoryImagePair m_layerSurfaceHistory;
    HistoryImagePair m_layerResult;
    bool m_layerReady = false;
    mutable bool m_layerInitialized = false;
    std::vector<VkDescriptorSet> m_layerDescriptorSets;
};
}
