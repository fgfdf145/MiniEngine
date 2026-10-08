#pragma once

// What every editor window is drawn with: the frame's inputs and outputs, the editor state the
// windows share, and the services that reach the other windows. The windows are views; the state
// they edit lives here or in the scene, not in one another.

#include <engine/editor/editor_commands.h>
#include <engine/editor/engine_settings.h>
#include <engine/editor/services/quad_recording.h>
#include <engine/editor/services/vehicle_drive_service.h>
#include <engine/editor/services/vehicle_driver_service.h>
#include <engine/editor/services/vehicle_rig_service.h>
#include <engine/platform/display/display_hdr.h>
#include <engine/renderer/camera.h>
#include <engine/renderer/rhi/backend.h>

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <optional>
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
    // The physics' fixed step as a rate, steps per simulated second.
    int physicsRateHz = static_cast<int>(1.0f / PhysicsWorld::kDefaultStepSeconds + 0.5f);
    VehiclePhysicsOverlaySettings overlay;
};

// The display the editor shows on, as the backend sees it each frame (the display calibration).
struct EditorDisplayStatus
{
    // What the OS reports about the display.
    platform::display::DisplayHdrInfo report;
    // Whether the output settings ask for HDR10, and whether the swapchain is HDR10.
    bool hdrRequested = false;
    bool hdrActive = false;
    // The values the output uses this frame.
    DisplayOutput output;
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
    platform::process::ProcessAllocation process;
    EditorVehicleSettings vehicle;

    // Set by the backend before each frame.
    std::string audioStatus;
    // The priority and CPUs the process runs at, or why they could not be set.
    std::string processStatus;
    VehicleDriveStatus vehicleStatus;
    VehicleRigStatus vehicleRigStatus;
    std::unordered_map<entt::entity, std::string> driverProblems;
    // The seated drivers' grip frames, and which wrist of the selected one the transform gizmo moves
    // instead of the entity (0 left, 1 right; -1 none).
    std::unordered_map<entt::entity, DriverGripFrames> driverGrips;
    int driverWristGizmo = -1;
    VideoRecordingIndicator videoRecording;
    // The Quad Recording window: the cameras and the video (kept in the engine settings), whether
    // the window shows their pictures this frame, and, from the backend, the recording and what the
    // cameras follow (the driven car, else the selection; empty with neither).
    QuadRecordingSettings quadRecording;
    bool quadRecordingPreview = false;
    VideoRecordingIndicator quadRecordingStatus;
    std::string quadRecordingTarget;
    ImTextureID minimapTexture = ImTextureID{};
    ImTextureID selectionOutlineTexture = ImTextureID{};
    bool dlssAvailable = false;
    bool dlssRayReconstructionAvailable = false;
    std::string dlssStatus;
    std::string gpuMemoryStatus;
    EditorDisplayStatus display;
    // The size the backend renders the scene at whatever the editor asks (--viewport-size, or a
    // recording's size while it runs); unset when the viewport's own settings decide.
    std::optional<RenderExtent> forcedViewportExtent;
    // The space the viewport panel had for its picture when it last drew docked, in points (its dock
    // node less the tab bar), for Window > Auto Layout; unset while it floats or is fullscreen.
    std::optional<ImVec2> viewportPanelArea;
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
