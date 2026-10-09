#pragma once

#include "compute_pass_util.h"
#include "scene_pass.h"
#include "uniform_buffer.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace me
{

class VulkanPathTraceLights;
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
//
// The offline mode (docs/design/2026-10-09-path-tracing-offline-mode-design.md) runs the same
// dispatches with several samples a pixel, the direct light traced at the G-buffer's surface, full
// float accumulations (the _float32 temporal and filter variants), and once the image has its samples
// no trace at all: the temporal pass carries the accumulations over (ScenePassFrameContext's
// pathTraceHold).
class VulkanPathTracePass : public IScenePass
{
  public:
    VulkanPathTracePass(
        VkDevice device,
        nvrhi::IDevice* nvrhiDevice,
        const SceneRenderTargets& targets,
        nvrhi::IBindingLayout* frameSetLayout,
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
    // The accumulations' format the next Prepare and PrepareLayer make: full float for the offline
    // mode, half otherwise. True when images of the other format exist: the caller waits for the frames
    // that may use them, calls ReleaseImages and points set 0 away from the layer's result.
    bool SetFullPrecisionHistory(bool fullPrecision);
    void ReleaseImages();
    // Makes the intermediate images at the targets' size when a frame is about to path trace and they
    // do not exist yet; true when it made them, and their history must be reset. Recording before
    // this for a path traced frame records nothing.
    bool Prepare(const SceneRenderTargets& targets);
    // The same for the forward-shaded surfaces' layer, read from the layer pass's images (which must
    // exist); true when it made them: the caller resets the layer's history, moves the result pair to
    // its resting layout (RecordLayerInitialTransition) and points set 0 at it.
    // shift: the layer is traced on a grid 2^shift times smaller each way than its G-buffer.
    bool PrepareLayer(const SceneRenderTargets& targets, const VulkanPathTraceLayerPass& layer, uint32_t shift);
    bool IsLayerReady() const;
    uint32_t GetLayerShift() const;
    // Frees the layer's images (the caller has waited for the frames that may use them); the next
    // PrepareLayer makes them again.
    void DestroyLayerImages();
    void RecordLayerInitialTransition(nvrhi::ICommandList* commandList) const;
    // The layer's traced light through the diffuse and the specular lobes, nearest, for set 0.
    TextureDescriptorBinding GetLayerDiffuseBinding() const;
    TextureDescriptorBinding GetLayerSpecularBinding() const;

  private:
    // The trace, and the temporal and filter dispatches the settings ask for, with one of the sets
    // (by layout: index 0 the trace's set 2, 1 the temporal and filter dispatches' set 1).
    void RecordPaths(
        nvrhi::ICommandList* commandList,
        const ScenePassFrameContext& frame,
        nvrhi::IBindingSet* traceSet,
        nvrhi::IBindingSet* passSet,
        std::span<nvrhi::ITexture* const> written,
        bool accumulate,
        bool denoise,
        bool historyValid,
        float historyScale,
        bool hitDistance,
        uint32_t gbufferShift,
        bool directLight,
        bool hold) const;
    void CreateRaw(VkExtent2D extent);
    // The sets over the images: one per layout for each transient copy and history read index (the
    // scene's), or each history read index (the layer's).
    struct Inputs
    {
        std::array<nvrhi::ITexture*, 8> gbuffer{};
        nvrhi::ITexture* final[2] = {};
    };
    void CreateBindingSets(const SceneRenderTargets& targets);
    void CreateLayerBindingSets(const VulkanPathTraceLayerPass& layer);
    nvrhi::BindingSetDesc DescribeSet(
        const Inputs& inputs,
        const HistoryImagePair& diffuse,
        const HistoryImagePair& specular,
        const HistoryImagePair& surface,
        uint32_t readIndex) const;
    void DestroyImages();

    VkDevice m_device = VK_NULL_HANDLE;
    nvrhi::IDevice* m_nvrhiDevice = nullptr;
    nvrhi::SamplerHandle m_nearestSampler;
    // The atmosphere's multiple-scattering LUT (binding 18), for the air along the paths. It rests in
    // GENERAL (VulkanAtmosphere); the dispatches read it as a shader resource.
    TextureDescriptorBinding m_multiScattering;
    // The emissive triangles as lights (the trace's set 4), built at the start of each path traced
    // frame; null without hardware ray tracing.
    std::unique_ptr<VulkanPathTraceLights> m_lights;
    // path_trace_common.slang's bindings 0-18, as the trace's set 2 and the other two shaders' set 1.
    nvrhi::BindingLayoutHandle m_traceSetLayout;
    nvrhi::BindingLayoutHandle m_setLayout;
    // The trace's variants, by [transmission][layered] (PT_TRANSMISSION, PT_LAYERED in path_trace.comp);
    // [1][1] is the whole of it. Frame set, ray set, this pass's set, ray texture table, emissive lights.
    nvrhi::ComputePipelineHandle m_tracePipelines[2][2];
    const VulkanRayScene* m_rayScene = nullptr;
    // By full precision (the offline mode's float32 accumulations). Frame set and this pass's set.
    nvrhi::ComputePipelineHandle m_temporalPipelines[2];
    nvrhi::ComputePipelineHandle m_filterPipelines[2];
    bool m_fullPrecision = false;
    // The raw paths (0 diffuse, 1 specular), rewritten every frame; and the accumulations of the two
    // channels and the surfaces they were made on, ping-ponged.
    HistoryImagePair m_raw;
    bool m_rawReady = false;
    HistoryImagePair m_diffuseHistory;
    HistoryImagePair m_specularHistory;
    HistoryImagePair m_surfaceHistory;
    bool m_imagesReady = false;
    // Indexed by transient copy * 2 + history read index; made once the images exist.
    std::vector<nvrhi::BindingSetHandle> m_traceSets;
    std::vector<nvrhi::BindingSetHandle> m_sets;
    // The scene's final targets each set writes (SceneGi, SceneReflections), by the same index.
    std::vector<std::array<nvrhi::ITexture*, 2>> m_finalTargets;
    // The layer's: its accumulations, its result (0 diffuse, 1 specular), its sets (by history read
    // index).
    HistoryImagePair m_layerDiffuseHistory;
    HistoryImagePair m_layerSpecularHistory;
    HistoryImagePair m_layerSurfaceHistory;
    HistoryImagePair m_layerResult;
    bool m_layerReady = false;
    uint32_t m_layerShift = 0;
    mutable bool m_layerInitialized = false;
    std::vector<nvrhi::BindingSetHandle> m_layerTraceSets;
    std::vector<nvrhi::BindingSetHandle> m_layerSets;
};
}
