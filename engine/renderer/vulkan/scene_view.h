#pragma once

#include "atmosphere.h"
#include "dlss.h"
#include "exposure_histogram_pass.h"
#include "gbuffer_inputs.h"
#include "path_trace_layer_pass.h"
#include "path_trace_pass.h"
#include "render_target_layout.h"
#include "restir_pt_pass.h"
#include "scatter_pass.h"
#include "scene_pass.h"
#include "scene_render_targets.h"
#include "shadow_pass.h"
#include "toon_pass.h"
#include "uniform_buffer.h"

#include <engine/renderer/exposure.h>
#include <engine/renderer/motion_history.h>
#include <engine/renderer/path_tracing.h>
#include <engine/renderer/temporal_history.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

// One camera the renderer draws the scene from in a frame: the viewport's, or one of a quad
// recording's (docs/design/2026-10-07-quad-vehicle-recording-design.md). Each has its own targets
// and passes, its own frame descriptor sets (set 0), shadow map, aerial perspective and clouds, and
// every history a temporal effect keeps, so no camera ever reads another's pixels. What the cameras
// share (the content, the ray scene, the probes, the LUTs) stays the renderer's.
struct VulkanSceneView
{
    std::unique_ptr<SceneRenderTargets> targets;
    // Set 2 and the set 1 filler for every pass that samples the G-buffer. Rebuilt with the passes
    // on a swapchain recreate, and rewritten before them on a resize.
    std::unique_ptr<VulkanGBufferDescriptors> gbufferDescriptors;
    // Every scene pass, owned, in construction order. Record order is decided per frame by
    // BuildScenePassOrder and resolved through FindPass, so this list is only ever walked whole
    // when the targets are rebuilt.
    std::vector<std::unique_ptr<IScenePass>> passes;
    // Non-owning, inside passes: the ones the renderer reads or prepares itself.
    VulkanExposureHistogramPass* exposurePass = nullptr;
    VulkanScatterPass* scatterPass = nullptr;
    VulkanPathTracePass* pathTracePass = nullptr;
    VulkanPathTraceLayerPass* pathTraceLayerPass = nullptr;
    VulkanRestirPtPass* restirPtPass = nullptr;
    // Set 0 per swapchain image: this camera's block, lights and clusters, previous model matrices,
    // and the bindings that follow this view's targets (scatter, clouds, aerial perspective,
    // shadow map).
    std::unique_ptr<VulkanUniformBuffer> uniformBuffer;
    // The directional light's cascades, fitted to this camera's frustum.
    std::unique_ptr<VulkanShadowPass> shadowPass;
    std::unique_ptr<VulkanAtmosphere::View> atmosphere;
    // The toon passes' per-frame materials, written from this view's draws.
    std::unique_ptr<VulkanToonMaterials> toonMaterials;
    // Scoped to one command buffer: reset where each frame records this view (see RenderFrame).
    RenderTargetLayoutTracker layoutTracker;
    // Last frame's matrices for motion vectors, and which history image each temporal pass reads
    // and writes. Reset wherever the targets are rebuilt (ResetHistories).
    MotionHistory motionHistory;
    TemporalHistory aoHistory;
    TemporalHistory rtShadowHistory;
    TemporalHistory giHistory;
    TemporalHistory ssrHistory;
    TemporalHistory taaHistory;
    TemporalHistory restirPtHistory;
    TemporalHistory pathTraceHistory;
    TemporalHistory pathTraceLayerHistory;
    // Whether the path traced image is standing still (the viewport's, or Photo Mode's view's), the
    // ray scene's install count it last saw (a new one is a scene change), and how far the offline
    // mode's accumulation is (set where it runs this frame).
    PathTraceAccumulation pathTraceAccumulation;
    uint32_t pathTraceGeometryEpoch = 0;
    std::optional<OfflineProgress> offlineProgress;
    // DLSS, where it resolves this view: the NGX feature it evaluates (the viewport's, or Photo
    // Mode's own at the photo's size and mode), whether that feature is made, and whether the next
    // evaluation drops DLSS's history.
    DlssFeatureSlot dlssSlot = DlssFeatureSlot::Viewport;
    bool dlssActive = false;
    bool dlssResetPending = true;
    // The pre-exposure the TAA and path tracing histories were written with; 0 before any frame.
    float taaHistoryPreExposure = 0.0f;
    float pathTraceHistoryPreExposure = 0.0f;
    float pathTraceLayerHistoryPreExposure = 0.0f;
    // Advances once per frame that jitters; picks the frame's offset in the TAA jitter sequence.
    uint32_t taaFrameIndex = 0;
    // Seeds the AO trace's noise; advances once per recorded frame.
    uint32_t aoFrameIndex = 0;
    // Last frame's camera, which ReSTIR PT's temporal reuse shifts paths to.
    glm::vec3 previousCameraPosition{0.0f};
    // Two-stage auto exposure (see StepAutoExposure), metered from this view's histograms, and the
    // EV it reached, which the next frame adapts from.
    AutoExposureState autoExposureState;
    std::optional<float> exposureEv100;
    bool hasMeteredExposure = false;

    IScenePass* FindPass(ScenePassId id) const
    {
        for (const std::unique_ptr<IScenePass>& pass : passes)
        {
            if (pass->Id() == id)
            {
                return pass.get();
            }
        }
        // A pass named by an order the renderer built but never constructed is a programming
        // error, and returning null here would surface as a crash inside recording instead.
        throw std::runtime_error("No scene pass is registered for scene pass id " + std::to_string(static_cast<int32_t>(id)));
    }

    // Every history starts over: the images behind them are new, or show something else.
    void ResetHistories()
    {
        layoutTracker.Reset();
        motionHistory.Reset();
        aoHistory.Reset();
        rtShadowHistory.Reset();
        pathTraceHistory.Reset();
        pathTraceLayerHistory.Reset();
        restirPtHistory.Reset();
        giHistory.Reset();
        ssrHistory.Reset();
        taaHistory.Reset();
        pathTraceAccumulation.Reset();
        offlineProgress.reset();
        dlssResetPending = true;
    }

    // Drops the passes and the per-image resources made from the targets (the pipelines built
    // against the passes' render passes go with them, see VulkanRenderer::CreateScenePasses).
    void ClearPasses()
    {
        passes.clear();
        exposurePass = nullptr;
        scatterPass = nullptr;
        pathTracePass = nullptr;
        pathTraceLayerPass = nullptr;
        restirPtPass = nullptr;
        gbufferDescriptors.reset();
    }
};
}
