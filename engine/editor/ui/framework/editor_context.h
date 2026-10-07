#pragma once

// What every editor window is drawn with: the frame's inputs and outputs, the editor state the
// windows share, and the services that reach the other windows. The windows are views; the state
// they edit lives here or in the scene, not in one another.

#include <engine/editor/editor_commands.h>
#include <engine/editor/engine_settings.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/editor/services/vehicle_rig_service.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/rhi/backend.h>

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace me
{

class CommandRegistry;
class EditorStyle;
class EditorWindowManager;
class IEditorWorld;
struct EditorUiFrameResult;

// What the viewport shows of a video recording (Tools > Record Viewport).
struct VideoRecordingIndicator
{
    bool active = false;
    // The video's length so far, its files' size and the frames left out while the encoders were
    // behind.
    double seconds = 0.0;
    uint64_t bytes = 0;
    uint64_t droppedFrames = 0;
    // How the last recording ended (where it was saved, or why it stopped), shown for a few seconds
    // from messageTime.
    std::string message;
    bool messageIsError = false;
    std::chrono::steady_clock::time_point messageTime{};
};

// The backend's per-frame inputs that only the windows read.
struct EditorFrameInput
{
    std::string lastLoadError;
    std::string lastSceneIoError;
    std::string sceneUploadStatus;
    ImTextureID viewportTextureId = ImTextureID{};
    RenderExtent viewportExtent{1, 1};
    RenderBackendType backendType{};
};

// The settings a driven car starts with, which the Vehicle panel edits and the viewport reads.
struct EditorVehicleSettings
{
    VehicleSettings tuning = VehicleDriveService::DefaultTuning();
    VehicleCameraSettings camera;
    VehicleHapticsSettings haptics;
    VehicleSteeringAssistSettings steeringAssist;
    bool manualGearbox = false;
    VehiclePhysicsOverlaySettings overlay;
};

// The editor state more than one window reads or writes, kept across frames by the editor shell.
struct EditorSharedState
{
    // The checkable commands' state (transform tool, play state, debug view, fullscreen, ...).
    EditorCommandState commands;
    // When the viewport last went fullscreen, for the hint it shows.
    double fullscreenEnteredTime = 0.0;

    // Graphics Debug: kept applying while its window is closed.
    RenderDebugSettings renderDebug;
    EngineAudioSettings audio;
    EditorVehicleSettings vehicle;

    // Set by the backend before each frame.
    std::string audioStatus;
    VehicleDriveStatus vehicleStatus;
    VehicleRigStatus vehicleRigStatus;
    std::unordered_map<entt::entity, std::string> driverProblems;
    VideoRecordingIndicator videoRecording;
    ImTextureID minimapTexture = ImTextureID{};
    ImTextureID selectionOutlineTexture = ImTextureID{};
    bool dlssAvailable = false;
    bool dlssRayReconstructionAvailable = false;
    std::string dlssStatus;
    std::string gpuMemoryStatus;
    // Whether the backend can path trace (hardware ray tracing), and what it says of path tracing.
    bool pathTracingAvailable = false;
    std::string pathTracingStatus;

    // DLSS resolves the viewport (see ResolveSceneExtents in the Vulkan renderer): it picks the render
    // size itself, so the viewport asks for every display pixel whatever the render scale says.
    bool DlssResolves() const
    {
        return dlssAvailable && renderDebug.dlssMode != DlssMode::Off && !renderDebug.forwardOnly;
    }
};

// One frame of the editor UI, as each window's OnGui sees it.
struct EditorContext
{
    IEditorWorld& scene;
    Camera& camera;
    ViewportMatrices& matrices;
    const EditorFrameInput& frame;
    // What the windows ask of the backend this frame.
    EditorUiFrameResult& result;
    EditorSharedState& state;
    // The UI scale and the theme.
    EditorStyle& style;
    // Opens, finds and focuses the other windows.
    EditorWindowManager& windows;
    const CommandRegistry& commands;
};
}
