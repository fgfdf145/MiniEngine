#pragma once

#include "editor_ui.h"
#include "engine_settings.h"
#include "services/vehicle_drive_service.h"
#include "services/vehicle_rig_service.h"
#include "services/world_streaming_service.h"

#include <engine/renderer/camera.h>
#include <engine/renderer/render_types.h>
#include <engine/renderer/renderer_world.h>

#include <engine/audio/audio_engine.h>
#include <engine/logic/editor_world.h>
#include <engine/core/input/input.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace me
{

struct ViewportDragPreviewState
{
    bool active = false;
    entt::entity entity = entt::null;
    entt::entity previousSelection = entt::null;
    std::string modelPath;
};

// A background task's future and what callers ask of it.
template <typename Result>
struct AsyncTask
{
    // Valid while the task runs, and after it finished until its result is consumed.
    std::future<Result> future;

    bool IsActive() const
    {
        return future.valid();
    }
    bool IsLoading() const
    {
        return future.valid() &&
               future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
};

// A background task that reports how far it got.
template <typename Result>
struct AsyncTaskWithProgress : AsyncTask<Result>
{
    // Overall fraction in [0, 1], written by the task's thread.
    std::shared_ptr<std::atomic<float>> progress;

    float Progress() const
    {
        return progress ? progress->load() : 0.0f;
    }
};

// State for a single in-flight async model parse.
// Main thread writes fields before starting; background thread reads them.
struct AsyncModelLoad : AsyncTaskWithProgress<void>
{
    // Path being loaded (set before thread starts, read-only in thread).
    std::string path;

    // Context needed to finalize placement / roll back on failure.
    bool isReplacement = false; // true = LoadSelectedModel, false = PlaceModelIntoScene
    glm::vec3 worldPosition{0.0f};
    entt::entity trackedEntity = entt::null;
    entt::entity previousSelection = entt::null;
    bool resetTransformOnComplete = false;
    std::string previousSourcePath;
    std::string previousSourceUuid;
    std::string previousDisplayName;
};

// State for a single in-flight async scene load.
// The background thread parses the scene file and pre-warms the model cache
// for every referenced model; the main thread applies the parsed data once ready.
struct AsyncSceneLoad : AsyncTaskWithProgress<SerializedSceneData>
{
    std::string path;
};

// State for a single in-flight asset import: the file copies run on a
// background thread so large models don't stall the UI frame.
// The future resolves to the imported model path, or throws.
struct AsyncAssetImport : AsyncTaskWithProgress<std::string>
{
    std::string sourcePath;
    std::string destinationDirectory;
};

// A model load queued from the UI. Loads are started one at a time as the
// async loader frees up, so batch requests don't get dropped.
struct PendingModelLoad
{
    std::string path;
    // Batch loads always create new entities; single loads keep the historical
    // behavior of replacing the current selection's model when there is one.
    bool placeAsNewEntity = false;
};

struct RendererSharedState
{
    IEditorWorld& GetEditorWorld()
    {
        if (!editorWorld)
        {
            throw std::runtime_error("RendererSharedState editor world has not been created");
        }

        return *editorWorld;
    }

    const IEditorWorld& GetEditorWorld() const
    {
        if (!editorWorld)
        {
            throw std::runtime_error("RendererSharedState editor world has not been created");
        }

        return *editorWorld;
    }

    bool initialized = false;
    bool renderablesDirty = false;
    // The render backend draws on a render thread, one frame behind the main thread; off
    // (--no-render-thread), it draws on the main thread inside DrawFrame.
    bool renderThread = true;
    InputState input;
    Camera camera;
    ViewportMatrices viewportMatrices;
    EditorUiController editorUi;
    std::unique_ptr<IEditorWorld> editorWorld;
    RendererWorld rendererWorld;
    ViewportDragPreviewState viewportDragPreview;
    AsyncModelLoad asyncLoad;
    AsyncSceneLoad asyncSceneLoad;
    AsyncAssetImport asyncImport;
    std::string lastModelLoadError;
    std::string lastSceneIoError;
    // Progress of a scene change waiting on background texture preparation, such as
    // "Preparing textures: 12 of 72"; empty when nothing is pending. Written by the render backend.
    std::string sceneUploadStatus;
    std::string lastEngineSettingsError;
    // The viewport recording, as the editor backend last saw it.
    VideoRecordingIndicator videoRecording;
    std::deque<PendingModelLoad> pendingModelLoads;
    std::optional<std::string> pendingScenePath;
    std::filesystem::path engineSettingsPath;
    EngineSettings engineSettings;
    bool engineSettingsNeedsBootstrapSave = false;
    // Set when the editor UI changed a persisted setting that has not been written yet.
    bool engineSettingsDirty = false;
    // The command line set the camera or the renderer (a capture state, a debug view, a scripted
    // run): the saved camera and render settings are neither applied nor overwritten, so a run is
    // reproducible and leaves the user's settings as they were.
    bool viewSettingsFromCommandLine = false;
    // Written by the render backend each frame: new content the render thread has not drawn yet, or
    // whose ray-traced scene (DDGI's) is still building. --wait-for-scene counts no frames until it
    // is done.
    bool rayScenePending = false;
    RenderExtent requestedViewportExtent{};
    // --viewport-size: the scene renders at this size whatever the viewport panel's size, so captures
    // do not depend on the editor's layout or the window manager. Unset, the panel decides.
    std::optional<RenderExtent> fixedViewportExtent;
    // Copied from the editor every frame in ApplyUiActions and read by the backend when it builds
    // the frame. The editor's copy is what the engine settings file saves (EngineViewSettings).
    RenderDebugSettings renderDebug;
    // Play mode: the model being driven as a car, if any (VehicleDriveService).
    VehicleDriveState vehicleDrive;
    // The selected car on the live seven-post rig, if it is running (VehicleRigService).
    VehicleRigState vehicleRig;
    // The scene's streamed worlds: which cells show their high detail or LOD (WorldStreamingService).
    WorldStreamingState worldStreaming;
    std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
    // Seconds between the last two TickSharedFrame calls; drives time-based effects such as
    // exposure adaptation.
    float frameDeltaSeconds = 0.0f;
    // The engine's sound, made by the application; null when there is no audio output (--no-audio,
    // or no playback device). The listener follows the camera (TickSharedFrame).
    std::unique_ptr<AudioEngine> audio;
    // What the Preferences window says of it: the device and format, or why there is none.
    std::string audioStatus;
};
}
