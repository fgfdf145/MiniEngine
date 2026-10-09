#pragma once

#include <engine/asset/model_loader.h>
#include <engine/audio/gamepad_haptics.h>
#include <engine/editor/services/vehicle_drive_log.h>
#include <engine/editor/services/vehicle_gear_shift.h>
#include <engine/editor/services/vehicle_haptics.h>
#include <engine/editor/services/vehicle_path_follower.h>
#include <engine/editor/services/vehicle_steering_assist.h>
#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>
#include <engine/renderer/camera.h>
#include <engine/scene/scene_components.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace me
{

class InputState;
class RendererWorld;
class ISceneWorld;
struct RendererSharedState;

// The cameras a driven car is seen from, in the order Gran Turismo 7's view button goes through them:
// behind the car, the driver's eyes, over the bonnet, and on the front bumper. All but the chase
// camera are fixed to the body, so they pitch and roll with it.
enum class VehicleCameraView : uint8_t
{
    Chase,
    Cockpit,
    Bonnet,
    Bumper,
};
inline constexpr size_t kVehicleCameraViewCount = 4;
// The cameras fixed to the body: every view but the chase camera, in order.
inline constexpr size_t kVehicleMountedViewCount = kVehicleCameraViewCount - 1;

const char* VehicleCameraViewName(VehicleCameraView view);
VehicleCameraView NextVehicleCameraView(VehicleCameraView view);

// The cameras on a driven car.
struct VehicleCameraSettings
{
    bool follow = true;
    float distance = 6.5f;   // metres behind the car
    float height = 2.0f;     // metres above the car's origin
    float lookHeight = 1.0f; // the point above the car's origin the camera looks at
    // Holding the right mouse button (or pushing the right stick) swings the camera round the car.
    // Letting go leaves it where it is when this is 0; above 0 it comes back behind the car at this
    // rate per second (higher is quicker).
    float lookRecenterRate = 0.0f;
    // The chase camera's vertical field of view (degrees) while driving.
    float chaseFovDegrees = 40.0f;
    // The views fixed to the body: their vertical field of view (degrees), the cockpit's own and the
    // bonnet's and bumper's. The lens is put back as it was on leaving the car.
    float cockpitFovDegrees = 55.0f;
    float exteriorFovDegrees = 50.0f;
    // Moves the driver's eyes from where the car's data or its steering wheel puts them (metres: to the
    // car's right, up, forward).
    glm::vec3 seatOffset{0.0f};
    // Looking round from inside the car (or off the bonnet or bumper) turns the head, and the head comes
    // back to the road at this rate per second once let go of, as GT7's does.
    float headLookRecenterRate = 12.0f;
};

// How far the driver has looked around with the right mouse button: for the chase camera, swung round
// the car from its place behind it; for a camera on the body, the head turned from looking ahead.
struct VehicleCameraOrbit
{
    float yawDegrees = 0.0f;   // positive swings the camera to the car's right, so the view turns left
    float pitchDegrees = 0.0f; // positive raises the camera, looking down on the car
};

// What the viewport draws over a driven car's suspension and tyres.
struct VehiclePhysicsOverlaySettings
{
    bool enabled = false;
    // The springs from the mounts to the wheels, and a gauge of how far each has travelled.
    bool suspension = true;
    // The tyres' outline and contact patch, coloured by how much of their grip is in use.
    bool tyres = true;
    // The load, drive and cornering forces at each contact patch.
    bool forces = true;
    // A friction circle and figures for each wheel, over a corner of the viewport.
    bool frictionCircles = true;
    // The multibody suspension's arms, rods, uprights and joints at each wheel, over the body.
    bool linkage = true;
    // The brush tyre's contact patch (VehicleTyreModel::Brush): each rib over its contact length, green
    // where its bristles stick and red where they slide, on the carcass's shifted, bent and twisted
    // centre line, with the patch at rest outlined. The carcass's deflection is drawn this many times
    // its size (a few millimetres would not show).
    bool contactPatch = true;
    float deformationScale = 10.0f;
    // How long an arrow is per kilonewton.
    float metresPerKilonewton = 0.15f;
};

// The car driving itself (docs/design/2026-10-09-drive-path-follow-design.md): along a drive path, or
// replaying a drive log.
enum class VehicleAutomationMode : uint8_t
{
    None,
    Path,
    Replay,
};

// What the Drive Paths panel and the viewport show of it.
struct VehicleAutomationStatus
{
    VehicleAutomationMode mode = VehicleAutomationMode::None;
    // The path's name, or the replayed log's file name.
    std::string name;
    PathFollowerStatus status = PathFollowerStatus::Running;
    std::string failure;
    // Along the path (m) and its length, the lap and the laps to drive; a replay's frame and frame count.
    double distance = 0.0;
    double length = 0.0;
    int lap = 0;
    int laps = 1;
    size_t frame = 0;
    size_t frames = 0;
    float lateralError = 0.0f;
    float targetKmh = 0.0f;
    // The path's point nearest the car and the point it steers for.
    glm::dvec3 closest{0.0};
    glm::dvec3 lookahead{0.0};
    // The drive log being written; empty when none.
    std::string logPath;
};

// What the Vehicle panel shows.
struct VehicleDriveStatus
{
    bool active = false;
    bool paused = false;
    std::string vehicleName;
    // What the car's own data set, in words; empty when the drive uses the tuning alone.
    std::string carData;
    VehicleTelemetry telemetry;
    // Where the body is (interpolated, as the car is drawn); its +Z is the car's front.
    PhysicsPose pose;
    // Front left, front right, rear left, rear right, as the last step left them, in world space.
    std::vector<VehicleWheelState> wheels;
    VehicleLinkage linkage;
    size_t staticBodyCount = 0;
    size_t staticTriangleCount = 0;
    // How much of real time the physics kept up with over the last second or so (1 is real time;
    // less when the build or the car is too slow and the drive runs in slow motion).
    float realTimeShare = 1.0f;
    // For the driving HUD: the controls the car took this frame (after the steering assist), the
    // gearbox, the rev limit and the revs the automatic changes up at on full throttle, which assists the car has and which of those are switched on, whether it
    // has turbos, the tyres' compound initials (front, rear; empty without the car's data) and how far
    // it has driven since the start.
    VehicleControls controls;
    bool manualGearbox = false;
    float engineMaxRpm = 7000.0f;
    float shiftUpRpm = 6500.0f;
    bool absFitted = false;
    bool tractionControlFitted = false;
    bool absOn = false;
    bool tractionControlOn = false;
    bool counterSteerAssist = false;
    bool turbo = false;
    std::string frontTyre;
    std::string rearTyre;
    double odometerMetres = 0.0;
    // The view the car is seen from (also while nothing is driven: the next drive starts in it).
    VehicleCameraView cameraView = VehicleCameraView::Chase;
    VehicleAutomationStatus automation;
    // The last path run or replay as it ended (also once the drive has stopped).
    std::string lastRunSummary;
    std::string lastError;
};

// How far the steering wheel turns each way at full lock when the car's data does not say: 900 degrees
// lock to lock, as a road car.
inline constexpr float kSteeringWheelLockDegrees = 450.0f;

// The model's wheels, tyres, suspension and steering wheel moving with the simulation: which
// submeshes belong to which wheel, and where each wheel's centre sits at rest, both as the model's
// WHEEL_xx, DISC_xx, SUSP_xx and STEER_HR nodes define them.
struct VehicleWheelAnimation
{
    // Keeps the submeshes' tags alive; the cache may drop the model while it is driven.
    std::shared_ptr<const LoadedModelData> model;
    std::array<glm::vec3, kModelWheelCornerCount> restCenters{};
    // The front wheels' steering at full lock, which turns the steering wheel by steeringWheelLockDegrees.
    float maxSteerDegrees = 35.0f;
    float steeringWheelLockDegrees = kSteeringWheelLockDegrees;
};

// The buttons that change gear, as held down.
struct VehicleGearButtons
{
    bool up = false;
    bool down = false;
};

// A drive path being followed: the path sampled with its speed plan, and the follower.
struct VehiclePathFollowRun
{
    DrivePathTrack track;
    PathFollowerSettings settings;
    PathFollowerState follower;
    PathFollowerOutput output;
};

// A drive log being played back: the body and wheels put where the log has them, time slice by time
// slice, while the physics stands still. Played back, a drive goes exactly where it went.
struct VehicleReplayRun
{
    std::string name;
    DriveReplay replay;
    // How far into the drive (s, on the log's time), the log's frame at or before it, and the drive then.
    double seconds = 0.0;
    size_t cursor = 0;
    DriveLogSample sample;
};

// A model being driven as a car: the physics world built for it, and what to put back when it stops.
struct VehicleDriveSession
{
    entt::entity entity = entt::null;
    std::string name;
    // What the car's own data set (DescribeCarSpec); empty when the drive uses the tuning alone.
    std::string carData;
    TransformComponent startTransform;
    // The vehicle body's pose (vehicle space, +Z forward), not the model's.
    PhysicsPose startPose;
    // How far the body's origin started above the ground under it: a car put upright where it lies
    // (Recover) is set down this high, plus a little to drop.
    float startHeightAboveGround = 0.0f;
    glm::vec3 scale{1.0f};
    // Turns vehicle space into the model's own: the entity's rotation is the body's times this.
    glm::quat vehicleToModel{1.0f, 0.0f, 0.0f, 0.0f};
    // The share of real time the physics has kept up with, smoothed over about a second.
    float realTimeShare = 1.0f;
    Camera cameraBeforeDriving;
    // Where the cockpit, bonnet and bumper cameras sit on the body (ComputeCameraMounts, before the
    // seat offset), in vehicle space.
    std::array<VehicleCameraMount, kVehicleMountedViewCount> cameraMounts{};
    // The lens the drive's views changed (field of view, near plane, up), to put back on leaving the car.
    bool driveLensApplied = false;
    float fovBeforeDriving = 45.0f;
    float nearPlaneBeforeDriving = 0.1f;
    glm::vec3 upBeforeDriving{0.0f, 1.0f, 0.0f};
    // The view button held in the last frame: the view changes once per press.
    bool viewButtonHeld = false;
    std::unique_ptr<PhysicsWorld> physics;
    VehicleId vehicle = 0;
    // Set when the model defines its wheels.
    std::optional<VehicleWheelAnimation> wheels;
    // How far the steering wheel has turned about its column (ModelSteeringWheel::axis), radians
    // (SteeringWheelTurn).
    float steeringWheelTurn = 0.0f;
    // A character driving the car (VehicleDriverService): its eyes in vehicle space, where the
    // cockpit camera sits instead of the car's own.
    std::optional<glm::vec3> driverEyes;
    // The gearbox's last change as the lever (and a driver's hand) make it.
    VehicleGearShift gearShift;
    bool paused = false;
    bool stepRequested = false;
    // The keyboard's steering, eased towards full lock rather than jumping to it.
    float keyboardSteering = 0.0f;
    // The steering assist's state, and what it needs of the car: the front wheels' full lock (degrees),
    // the wheelbase (m) and the front tyres' peak slip angle (degrees).
    VehicleSteeringAssistState steeringAssist;
    float maxSteerDegrees = 35.0f;
    float wheelbase = 2.6f;
    // The rear axle along the body's forward axis from its origin (m): where a path run steers from.
    float rearAxleZ = -1.3f;
    float frontPeakSlipDegrees = 7.0f;
    bool resetHeld = false;
    bool recoverHeld = false;
    // The gear buttons held in the last frame: a change is made once per press.
    VehicleGearButtons gearButtonsHeld;
    // Under scripted controls: the simulated time so far, and when the pose is next logged.
    float scriptedSeconds = 0.0f;
    float nextScriptedLogSeconds = 0.0f;
    double scriptedPhysicsMs = 0.0; // wall time spent stepping since the last log
    VehicleCameraOrbit orbit;
    // The engine's idle and rev limit, which the gamepad's rumble places the revs between.
    float engineMinRpm = 1000.0f;
    float engineMaxRpm = 7000.0f;
    // Where the automatic changes up on full throttle (VehicleShiftPoints::upFull): the HUD's shift light.
    float shiftUpRpm = 6500.0f;
    VehicleHapticsState haptics;
    // A DualSense on USB: its actuators play the engine, the road and the tyres (GamepadHaptics). Opened
    // once per connection of the pad, as finding its device takes a moment and over Bluetooth there is none.
    std::unique_ptr<GamepadHaptics> audioHaptics;
    bool audioHapticsTried = false;
    VehicleAudioHapticsState audioHapticsState;
    // The engine's beat on the actuators follows its firing: AC's data names no cylinder count, and the
    // R34's RB26 has six.
    int engineCylinders = 6;
    // What the driving HUD shows of the car (see VehicleDriveStatus).
    VehicleControls controls;
    bool absFitted = false;
    bool tractionControlFitted = false;
    // The driver aids' switches (SetDriverAids), and their buttons held in the last frame: each press
    // switches once.
    bool absOn = true;
    bool tractionControlOn = true;
    bool absButtonHeld = false;
    bool tractionControlButtonHeld = false;
    bool turbo = false;
    std::string frontTyre;
    std::string rearTyre;
    double odometerMetres = 0.0;
    // The car driving itself: along a drive path or replaying a log (one at most), and the drive written
    // down. A run's figures are gathered while either goes on, until it ends (runEnded), and while a
    // drive by hand is written down.
    std::optional<VehiclePathFollowRun> pathFollow;
    std::optional<VehicleReplayRun> replay;
    DriveLogWriter log;
    DriveRunStats runStats;
    double runSeconds = 0.0;
    bool runEnded = false;
    // How long all four wheels have been on the ground since the run began: its figures count once the
    // car has settled from being set down (kRunSettleSeconds), not the bump of landing.
    float runSettledSeconds = 0.0f;
    // The body's velocity and heading after the last logged frame, for its accelerations and yaw rate.
    std::optional<glm::vec3> lastVelocity;
    float lastYaw = 0.0f;
    double lastHeight = 0.0;
};

struct VehicleDriveState
{
    std::unique_ptr<VehicleDriveSession> session;
    // A sequential manual gearbox (the driver changes gear) instead of the automatic.
    bool manualGearbox = false;
    // The physics' fixed step (PhysicsWorld::SetStepSeconds), which a car being driven takes at once.
    float physicsStepSeconds = PhysicsWorld::kDefaultStepSeconds;
    // A test drive without a driver (--drive-controls): the car takes these controls instead of the
    // keyboard's and gamepad's, advances a fixed 1/60 s a frame however long the frame took, and logs
    // its pose once a simulated second.
    std::optional<VehicleControls> scriptedControls;
    // A path run or replay advances a fixed 1/60 s a frame however long the frame took (a scripted run);
    // off, it keeps to real time.
    bool fixedFrameStep = false;
    // How the last path run or replay ended: true finished, false failed; unset while none has. A
    // scripted run stops once it is set.
    std::optional<bool> automationResult;
    std::string lastRunSummary;
    VehicleCameraSettings camera;
    // The view the car is seen from; it stays for the next drive.
    VehicleCameraView cameraView = VehicleCameraView::Chase;
    VehicleHapticsSettings haptics;
    VehicleSteeringAssistSettings steeringAssist;
    std::string lastError;
};

namespace VehicleDriveService
{
// Turns vehicle space (+Z forward, +X the car's left) into the model's own for a model facing `front`.
glm::quat VehicleToModelRotation(VehicleModelFront front);
// The way a model faces, from where its wheels are: the front wheels' Z against the rear's.
VehicleModelFront ModelFrontFromWheels(const ModelWheelRig& rig);
// The Vehicle panel's tuning before the user changes it: the defaults, on the brush tyre.
VehicleSettings DefaultTuning();
// Drives this model entity as a car: a physics world is built from every other loaded model's
// triangles (the track) and a ground plane under the lowest of them, and a car fitted to the
// entity's bounds is placed at its transform. Throws when the entity is not a model.
void Start(RendererSharedState& state, entt::entity entity, const VehicleSettings& tuning);
// Puts the car back where it started and the camera where it was.
void Stop(RendererSharedState& state);
// Puts the car back where it started, stopped, and keeps driving.
void Reset(RendererSharedState& state);
// Puts the car back on its wheels where it is, facing the way it was heading, stopped: for a car
// that has rolled over or got stuck on its side.
void Recover(RendererSharedState& state);
void SetPaused(RendererSharedState& state, bool paused);
// Sees the car from `view`, the head looking ahead again; the next drive starts in it too.
void SetCameraView(RendererSharedState& state, VehicleCameraView view);
// The driven car's brush tyres recut into this many ribs and segments along each (0 for the tyre's own
// count), at once.
void SetBrushTyreBristles(RendererSharedState& state, int ribs, int segmentsPerRib);
// Switches the driven car's anti-lock brakes and traction control on or off, at once.
void SetDriverAids(RendererSharedState& state, bool abs, bool tractionControl);
// While paused: advances the simulation by one fixed step.
void Step(RendererSharedState& state);

// The car drives itself along the scene's drive path `name` (ComputePathFollowControls): it is put on
// the path's start facing along it, which Reset then goes back to. Throws when nothing is driven, or
// the scene has no such path or it has fewer than two points.
void StartPathFollow(
    RendererSharedState& state, const std::string& name, const DrivePathTrackSettings& track = {}, const PathFollowerSettings& follower = {});
// The car replays a drive log (ReadDriveLog): put back where that drive started, at its physics step,
// it is given the controls and steps of each of its frames. Throws when nothing is driven or the log
// cannot be read.
void StartReplay(RendererSharedState& state, const std::filesystem::path& path);
// Ends a path run or replay; the keyboard and gamepad drive the car again.
void StopAutomation(RendererSharedState& state);
// Writes the drive down from now (DriveLogWriter). With `fromStart` the car first goes back to its
// start (Reset), so the log can be replayed from where it starts. Throws when nothing is driven or the
// file cannot be written.
void StartDriveLog(RendererSharedState& state, const std::filesystem::path& path, bool fromStart);
void StopDriveLog(RendererSharedState& state);

// Per frame: reads the driver's input, advances the simulation, and moves the car's entity and
// the chase camera. False when nothing is being driven. Stops driving once the entity is gone.
bool Tick(RendererSharedState& state, float deltaSeconds, bool keyboardCaptured);

VehicleDriveStatus GetStatus(const RendererSharedState& state);

// Runs `action` with the driven car back at its start transform, and returns it to the road after:
// a scene saved while driving records where the car was placed, not where it was driven to.
void RunWithVehicleAtStart(RendererSharedState& state, const std::function<void()>& action);

// Keyboard: W/S or the arrow keys for throttle and reverse, A/D or left/right to steer, Space for the
// hand brake, E/Q to change up/down and N (held) for the clutch (V and the right stick's click change
// the view, B and the D-pad's left switch ABS, T and the D-pad's right traction control, which Tick
// reads). Gamepad: right and left trigger, left
// stick, East button (Circle on a DualSense) for the hand brake, right/left shoulder (R1/L1) to change
// up/down and South (Cross, held) for the clutch (with `manualGearbox`). The gear buttons change gear once per press,
// the automatic's too (it holds the gear a while after):
// `gearButtonsHeld` is what was held the frame before, carried between frames like `keyboardSteering`,
// the eased keyboard steering.
VehicleControls ReadVehicleControls(const InputState& input, bool keyboardCaptured, float deltaSeconds, float& keyboardSteering,
                                    bool manualGearbox = false, VehicleGearButtons* gearButtonsHeld = nullptr);

// Adds the static collision for every loaded model except `exclude`, in world space. A model with
// collision meshes of its own (an Assetto Corsa track's physics meshes, each surface at its friction)
// collides through those alone; any other through its opaque and alpha-tested submeshes' triangles,
// less the glass, decals and ground cover (IsGroundCover) a car should drive over. Returns the lowest
// vertex height, or `fallbackFloor` when nothing was added.
float AddSceneCollision(PhysicsWorld& physics, const RendererWorld& renderWorld, const ISceneWorld& scene, entt::entity exclude, float fallbackFloor);

// How far the steering wheel has turned about its column (ModelSteeringWheel::axis), radians, for
// the front wheels at `wheels`: clockwise for the driver, looking along the column, when the car
// steers right, and steeringWheelLockDegrees at the front wheels' full lock.
float SteeringWheelTurn(
    const VehicleWheelAnimation& animation,
    const PhysicsPose& body,
    const std::vector<VehicleWheelState>& wheels,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale);

// The local transform of each of the model's submeshes (in the order of its submeshes) for a car
// whose wheels are at `wheels`: the wheel's own parts steer, roll and ride the suspension, its brake
// disc steers and rides, its suspension only rides, the steering wheel turns with the front wheels;
// every other submesh stays. `body` and `wheels`
// are as PhysicsWorld reports them.
std::vector<glm::mat4> BuildWheelSubmeshTransforms(
    const VehicleWheelAnimation& animation,
    const PhysicsPose& body,
    const std::vector<VehicleWheelState>& wheels,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale);

// Each of the model's tyres (ModelSubmeshData::tyre) squashed where its wheel touches the ground and
// bent with its carcass: two matrices per submesh (PackTyreDeformation), an inactive pair for every
// other submesh and for a wheel in the air. `entityMatrix` is the entity's transform and
// `submeshTransforms` the submeshes' local ones (BuildWheelSubmeshTransforms), which carry each tyre
// from its shape at rest to the wheel. Empty for a model without tyres.
std::vector<glm::mat4> BuildTyreDeformations(
    const LoadedModelData& model,
    const std::vector<VehicleWheelState>& wheels,
    const glm::mat4& entityMatrix,
    const std::vector<glm::mat4>& submeshTransforms);

// Turns the orbit by a mouse movement (degrees), or, with `lookHeld` false, eases it back to zero at
// `recenterRate` per second (0: it stays). Pitch is kept between looking a little up from below and
// straight down.
void UpdateCameraOrbit(VehicleCameraOrbit& orbit, bool lookHeld, float yawDeltaDegrees, float pitchDeltaDegrees, float recenterRate, float deltaSeconds);

// Where the cockpit, bonnet and bumper cameras sit on a car, in vehicle space (+Z forward, +X to the
// car's left, from the body's origin), from what is known of it:
// - the cockpit: the driver's eyes from the car's data (`eyes`, vehicle space), else 0.59 m behind and
//   0.29 m over the steering wheel's centre (`steeringWheel`, as the R34's DRIVEREYES sit from its
//   STEER_HR), else on the centre line where a driver's eyes sit in the body's box.
// - the bonnet: on the centre line, 45% of the way from the eyes to the front, a little over the
//   highest point of the body there (`heightAt`, nullopt where there is none: then under the eyes).
// - the bumper: just ahead of the front, low, as GT7's.
// `boundsMin` and `boundsMax` are the body's box in vehicle space.
std::array<VehicleCameraMount, kVehicleMountedViewCount> ComputeCameraMounts(
    const glm::vec3& boundsMin,
    const glm::vec3& boundsMax,
    const std::optional<VehicleCameraMount>& eyes,
    const std::optional<glm::vec3>& steeringWheel,
    const std::function<std::optional<float>(float z)>& heightAt);

// Puts the camera at `mount` on the body (vehicle space), looking along the body's forward pitched by
// the mount, the head turned by `look` (positive yaw looks left, positive pitch down); its up is the
// body's, so it rolls with the car.
void UpdateMountedCamera(Camera& camera, const PhysicsPose& vehiclePose, const VehicleCameraMount& mount, const VehicleCameraOrbit& look = {});

// Eases the camera towards its place behind the car, looking at it, turned round the car by `orbit`.
// With deltaSeconds of zero it jumps straight there.
void UpdateChaseCamera(
    Camera& camera,
    const PhysicsPose& vehiclePose,
    const VehicleCameraSettings& settings,
    float deltaSeconds,
    const VehicleCameraOrbit& orbit = {});
}
}
