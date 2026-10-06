#pragma once

#include <engine/core/paths/engine_paths.h>
#include <engine/core/render_backend_type.h>
#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace me
{

struct EditorApplicationOptions
{
    RenderBackendType renderBackend = GetDefaultRenderBackendType();
    std::optional<std::string> startupModelPath;
    // A scene file loaded in place of the two-cube test scene, the way File > Open would load it.
    std::optional<std::string> startupScenePath;
    uint32_t maxFrames = 0;
    // With --frames: the viewport of the last frame is written here as a PNG.
    std::optional<std::string> capturePath;
    // --capture-at F1,F2,...: also captures the viewport after these counted frames, to the --capture
    // path with _F before its extension (convergence measurements).
    std::vector<uint32_t> captureFrames;
    // --turn-sun FRAME,PITCH,YAW,ROLL: after that counted frame, adds these degrees to every
    // directional light's rotation (a lighting change for convergence measurements).
    std::optional<std::array<float, 4>> sunTurn;
    // --record FILE.mp4 (H.264, Windows) or FILE.avi (MJPEG): a video of the viewport, one frame for every frame drawn from the second that
    // counts toward --frames, at --record-fps frames a second (30 by default).
    std::optional<std::filesystem::path> recordPath;
    uint32_t recordFramesPerSecond = 30;
    // Starts with the Khronos reference view on (Graphics Debug), for comparing captures against the
    // Khronos glTF Sample Viewer.
    bool khronosReference = false;
    // --viewport-size WxH: renders the scene at a fixed size, independent of the editor's layout.
    std::optional<RenderExtent> viewportSize;
    // For captures that need a view the scene file does not hold. --camera x,y,z,yaw,pitch places the
    // camera (metres, degrees); --camera-velocity x,y,z then moves it this far every frame, so a
    // capture of a moving camera does not depend on frame times.
    std::optional<std::array<float, 5>> camera;
    glm::vec3 cameraVelocity{0.0f};
    // --debug-view N starts with Graphics Debug's G-buffer view N (GBufferDebugView); --no-ddgi with
    // the DDGI probes off.
    std::optional<GBufferDebugView> debugView;
    bool ddgiDisabled = false;
    // --ddgi-spacing METRES: the finest DDGI level's probe spacing (DdgiSettings::baseSpacing).
    std::optional<float> ddgiSpacing;
    // --reference PREFIX: after the last frame, compares the DDGI irradiance (with --debug-view 15)
    // against a CPU path tracer on every --reference-stride-th pixel, --reference-samples paths each.
    std::optional<std::string> referencePrefix;
    uint32_t referenceSamples = 256;
    uint32_t referenceStride = 8;
    // --reference-explain COLUMN,ROW: logs that comparison point's probe lookup probe by probe.
    int referenceExplainColumn = -1;
    int referenceExplainRow = -1;
    // --state FILE: an editor capture's state (CaptureStateService): its scene snapshot, camera,
    // viewport size and Graphics Debug settings, applied before the other options, which override it.
    // Implies --wait-for-scene.
    std::optional<std::filesystem::path> statePath;
    // --wait-for-scene: --frames counts only frames drawn once the scene has loaded, its textures are
    // prepared and its ray scene is built, so a large scene is not captured half loaded.
    bool waitForScene = false;
    // --drive TAG: once the scene has loaded, drives the model entity with this name, as Play would.
    // --drive-controls THROTTLE,STEERING: holds these controls (-1 to 1) instead of reading the
    // keyboard and gamepad, a fixed 1/60 s of simulation a frame, logging the car's pose every
    // simulated second: a headless test drive (VehicleDriveState::scriptedControls).
    std::optional<std::string> driveEntity;
    std::optional<std::array<float, 2>> driveControls;
    // --no-audio: opens no playback device. Scripted runs (--frames) open none either: their sounds
    // are mixed into nothing.
    bool audioDisabled = false;
    // --no-render-thread: the render work runs on the main thread, inside each frame, as it did before
    // the render thread (for comparisons and debugging).
    bool renderThread = true;
    // --task-threads N: the task system's worker threads; 0 takes the logical processors less two.
    uint32_t taskThreads = 0;
    EnginePaths::Overrides paths;
};

class EditorApplication final
{
  public:
    static EditorApplicationOptions ParseArgs(int argc, char** argv);
    static void PrintDependencyLinkStatus();

    explicit EditorApplication(EditorApplicationOptions options);
    int Run();

  private:
    EditorApplicationOptions m_options;
};
}
