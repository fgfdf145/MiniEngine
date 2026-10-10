#pragma once

#include <engine/core/paths/engine_paths.h>
#include <engine/core/render_backend_type.h>
#include <engine/platform/process/process_allocation.h>
#include <engine/renderer/render_types.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace me
{

enum class VehicleCameraView : uint8_t;

struct EditorApplicationOptions
{
    // --adopt-car-tyres <car.gltf> <game folder>: put the tyres of a car imported before the tyre library
    // into it and refer the car to them, then exit (docs/design/2026-10-08-tyre-library-design.md).
    std::vector<std::pair<std::string, std::string>> adoptCarTyres;
    RenderBackendType renderBackend = GetDefaultRenderBackendType();
    // --backend named it; otherwise the Preferences window's saved choice decides at start.
    bool renderBackendFromArgs = false;
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
    // --quad-record FILE.mp4 or FILE.avi: films the driven car (--drive), else the selected model, from
    // four sides at once into one video, its cameras as the Quad Recording window last saved them;
    // starts and paces as --record does, at --record-fps.
    std::optional<std::filesystem::path> quadRecordPath;
    // --photo FILE.png: Photo Mode's still from the camera at --photo-size WxH (else the Photo Mode
    // window's saved size), started once the scene is ready, its view rendering --photo-warmup frames
    // (else the window's) before it is saved; the viewport keeps its own size. --frames must outlast it.
    std::optional<std::filesystem::path> photoPath;
    std::optional<RenderExtent> photoSize;
    std::optional<uint32_t> photoWarmupFrames;
    // --photo-max-view-pixels N: tiles the photo when it has more pixels than N, whatever the GPU
    // has free (for comparing tiled photos with whole ones).
    std::optional<uint64_t> photoMaxViewPixels;
    // --photo-at N: presses the shutter on the Nth frame --frames counts rather than the first, so the
    // viewport's auto exposure, which the photo keeps, has adapted.
    uint32_t photoAtFrame = 1;
    // How the photo renders, else as the Photo Mode window saved it: --photo-path-tracing on|off,
    // --photo-spp N (samples a frame), --photo-samples N (in all), --photo-dlss
    // off|dlaa|quality|balanced|performance|ultra-performance, --photo-rr on|off.
    std::optional<bool> photoPathTracing;
    std::optional<uint32_t> photoSamplesPerPixel;
    std::optional<uint32_t> photoTargetSamples;
    std::optional<DlssMode> photoDlssMode;
    std::optional<bool> photoRayReconstruction;
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
    // --display-pattern N,LEVEL shows the HDR calibration's pattern N (CalibrationPattern) at LEVEL
    // cd/m^2 in place of the scene, to measure what reaches the display.
    std::optional<DisplayCalibrationView> displayPattern;
    bool ddgiDisabled = false;
    // --software-rays: the ray scene's compute walk even where hardware ray tracing exists.
    bool softwareRays = false;
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
    // --drive-view chase|cockpit|bonnet|bumper: the view the drive is seen from; fixed: the camera stays
    // where --camera (or the scene) puts it and does not follow the car.
    std::optional<VehicleCameraView> driveView;
    bool driveCameraFixed = false;
    // --follow-path NAME: with --drive, the car follows the scene's drive path NAME by itself
    // (VehicleDriveService::StartPathFollow), a fixed 1/60 s a frame; the run ends with the path, exit
    // code 3 when the car fails it. --path-speed-scale S: every speed the path sets times S.
    std::optional<std::string> followPath;
    float pathSpeedScale = 1.0f;
    // --replay-drive FILE: with --drive, the car replays a drive log (VehicleDriveService::StartReplay);
    // the run ends with it.
    std::optional<std::filesystem::path> replayDrive;
    // --drive-log FILE: with --drive, writes the drive down as CSV (DriveLogWriter), from the path's start
    // or the replay's, else from the car's.
    std::optional<std::filesystem::path> driveLog;
    // --physics-rate HZ: the physics' fixed steps per second (the Vehicle panel's Physics Rate), 60 to 4000.
    std::optional<int> physicsRateHz;
    // --material-editor MODEL: opens the Material Editor on the model (its paint selected) at startup.
    std::optional<std::string> materialEditorModel;
    // --no-audio: opens no playback device. Scripted runs (--frames) open none either: their sounds
    // are mixed into nothing.
    bool audioDisabled = false;
    // --no-render-thread: the render work runs on the main thread, inside each frame, as it did before
    // the render thread (for comparisons and debugging).
    bool renderThread = true;
    // --no-parallel-recording: the render thread records every draw itself (comparisons).
    bool parallelRecording = true;
    // --no-ray-query: the device leaves hardware ray tracing off, as a GPU without it would.
    bool rayQuery = true;
    // --task-threads N: the task system's worker threads; 0 takes the CPUs the process was given
    // (--cpus, or the Preferences window's) less two.
    uint32_t taskThreads = 0;
    // --priority below-normal|normal|above-normal|high and --cpus all|performance|LIST (LIST as
    // 0,2,4-7): for this run, over the Preferences window's Process settings (High, all CPUs by default).
    std::optional<platform::process::ProcessPriority> processPriority;
    std::optional<platform::process::CpuSelection> cpuSelection;
    std::vector<uint32_t> customCpus;
    // --control [PORT]: the control channel (ControlSession) listens on 127.0.0.1:PORT (47811 when the
    // next argument is not a number); a client drives the editor while it runs. MINIENGINE_CONTROL_PORT
    // does the same.
    std::optional<uint16_t> controlPort;
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
    // Sets the saved priority and CPUs (the Preferences window's), as --priority and --cpus change
    // them; returns the CPUs the process runs on.
    std::vector<uint32_t> ApplyStartupProcessAllocation();

    EditorApplicationOptions m_options;
    // What ApplyStartupProcessAllocation did, for the Preferences window.
    std::string m_processStatus;
};
}
