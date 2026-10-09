#pragma once

#include "uniform_buffer.h"

#include <engine/editor/imgui_frame_snapshot.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/render_transform_snapshot.h>
#include <engine/renderer/render_types.h>
#include <engine/renderer/renderer_world.h>
#include <engine/renderer/scene_capture_view.h>
#include <engine/renderer/scene_lighting.h>
#include <engine/scene/scene_environment.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me
{

// Every light in the scene, in scene order, as the shader wants it and as the light selection
// ranks it. The two vectors are parallel.
struct CollectedSceneLights
{
    std::vector<GpuLightData> gpuLights;
    std::vector<SceneLightCandidate> candidates;
};

// One frame as the render thread draws it, built by the main thread at the end of its frame
// (VulkanRenderer::BuildFramePacket). While it draws, the render thread reads nothing of the editor's
// state but this: the main thread is already changing that for the next frame.
struct RenderFramePacket
{
    // Counts the frames built, from 1.
    uint64_t serial = 0;
    float deltaSeconds = 0.0f;
    // The render thread writes the exposure and white point it adapted into its copy, which the
    // feedback carries back.
    Camera camera;
    ViewportMatrices viewportMatrices;
    RenderDebugSettings renderDebug;
    // The size the scene renders at this frame.
    RenderExtent viewportExtent;
    // The window's display in pixels: the largest the viewport can become (fullscreen), which the GPU
    // memory report reserves room for. The viewport's own size when it has a fixed one.
    RenderExtent displayExtent;
    SceneEnvironment environment;
    // SceneMinimap::image when the minimap is valid, else empty.
    std::string minimapPath;
    CollectedSceneLights lights;
    // The editor's selection, which the selection outline draws around; entt::null without one.
    entt::entity selectedEntity = entt::null;
    // The editor UI's scale, which the outline's width follows.
    float uiScale = 1.0f;
    // The output's pixels per display pixel where the viewport shows them (EditorUiFrameResult).
    float viewportOutputScale = 1.0f;
    // RendererSharedState::temporalRestart: a change starts every temporal effect over.
    uint32_t temporalRestart = 0;
    // The renderables changed since the last frame: the render thread starts an upload.
    bool contentChanged = false;
    std::shared_ptr<const CpuRenderSubmeshList> renderSubmeshes;
    RenderTransformSnapshot transforms;
    ImGuiFrameSnapshot ui;
    // The quad recording's cameras this frame, in the canvas's order; none while nothing films.
    std::vector<SceneCaptureView> captureViews;
};

// What the render thread learned drawing a frame, for the main thread's next one.
struct RenderFeedback
{
    // The last frame the render thread finished with, drawn or not; 0 before the first.
    uint64_t serial = 0;
    float exposureEv100 = 0.0f;
    float adaptedLongTermEv100 = 0.0f;
    float adaptedWhiteKelvin = 0.0f;
    // As RendererSharedState::sceneUploadStatus.
    std::string sceneUploadStatus;
    bool rayScenePending = false;
    // Set once when an upload ran out of GPU memory (true) or a later one succeeded (false); the
    // main thread takes it.
    std::optional<bool> outOfMemory;
    bool minimapLoaded = false;
    GpuMemoryReport gpuMemory;
    // What path tracing is doing, for the Graphics Debug window: how far a still image has
    // accumulated, or why it does not run; and the offline mode's share of its target samples
    // (negative where it is not running).
    std::string pathTracingStatus;
    float pathTracingProgress = -1.0f;
};
}
