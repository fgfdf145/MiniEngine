#pragma once

#include <engine/asset/model_loader.h>
#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>
#include <engine/renderer/camera.h>
#include <engine/scene/scene_components.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace me
{

class InputState;
class RendererWorld;
class ISceneWorld;
struct RendererSharedState;

// The camera behind a driven car.
struct VehicleCameraSettings
{
    bool follow = true;
    float distance = 6.5f;   // metres behind the car
    float height = 2.0f;     // metres above the car's origin
    float lookHeight = 1.0f; // the point above the car's origin the camera looks at
    // How quickly the camera catches up with the car, per second; higher is stiffer.
    float stiffness = 6.0f;
    // Holding the right mouse button swings the camera round the car; letting go brings it back
    // behind it at this rate per second (higher is quicker).
    float lookRecenterRate = 4.0f;
};

// How far the driver has looked around the car with the right mouse button, added to the chase
// camera's place behind it.
struct VehicleCameraOrbit
{
    float yawDegrees = 0.0f;   // positive swings the camera to the car's right, so the view turns left
    float pitchDegrees = 0.0f; // positive raises the camera, looking down on the car
};

// What the Vehicle panel shows.
struct VehicleDriveStatus
{
    bool active = false;
    bool paused = false;
    std::string vehicleName;
    VehicleTelemetry telemetry;
    size_t staticBodyCount = 0;
    size_t staticTriangleCount = 0;
    std::string lastError;
};

// The model's wheels, tyres, suspension and steering wheel moving with the simulation: which
// submeshes belong to which wheel, and where each wheel's centre sits at rest, both as the model's
// WHEEL_xx, DISC_xx, SUSP_xx and STEER_HR nodes define them.
struct VehicleWheelAnimation
{
    // Keeps the submeshes' tags alive; the cache may drop the model while it is driven.
    std::shared_ptr<const LoadedModelData> model;
    std::array<glm::vec3, kModelWheelCornerCount> restCenters{};
    // The front wheels' steering at full lock, which turns the steering wheel by kSteeringWheelLockDegrees.
    float maxSteerDegrees = 35.0f;
};

// How far the steering wheel turns each way at full lock: 900 degrees lock to lock, as a road car.
inline constexpr float kSteeringWheelLockDegrees = 450.0f;

// A model being driven as a car: the physics world built for it, and what to put back when it stops.
struct VehicleDriveSession
{
    entt::entity entity = entt::null;
    std::string name;
    TransformComponent startTransform;
    // The vehicle body's pose (vehicle space, +Z forward), not the model's.
    PhysicsPose startPose;
    glm::vec3 scale{1.0f};
    // Turns vehicle space into the model's own: the entity's rotation is the body's times this.
    glm::quat vehicleToModel{1.0f, 0.0f, 0.0f, 0.0f};
    Camera cameraBeforeDriving;
    std::unique_ptr<PhysicsWorld> physics;
    VehicleId vehicle = 0;
    // Set when the model defines its wheels.
    std::optional<VehicleWheelAnimation> wheels;
    bool paused = false;
    bool stepRequested = false;
    // The keyboard's steering, eased towards full lock rather than jumping to it.
    float keyboardSteering = 0.0f;
    bool resetHeld = false;
    VehicleCameraOrbit orbit;
};

struct VehicleDriveState
{
    std::unique_ptr<VehicleDriveSession> session;
    VehicleCameraSettings camera;
    std::string lastError;
};

namespace VehicleDriveService
{
// Drives this model entity as a car: a physics world is built from every other loaded model's
// triangles (the track) and a ground plane under the lowest of them, and a car fitted to the
// entity's bounds is placed at its transform. Throws when the entity is not a model.
void Start(RendererSharedState& state, entt::entity entity, const VehicleSettings& tuning);
// Puts the car back where it started and the camera where it was.
void Stop(RendererSharedState& state);
// Puts the car back where it started, stopped, and keeps driving.
void Reset(RendererSharedState& state);
void SetPaused(RendererSharedState& state, bool paused);
// While paused: advances the simulation by one fixed step.
void Step(RendererSharedState& state);

// Per frame: reads the driver's input, advances the simulation, and moves the car's entity and
// the chase camera. False when nothing is being driven. Stops driving once the entity is gone.
bool Tick(RendererSharedState& state, float deltaSeconds, bool keyboardCaptured);

VehicleDriveStatus GetStatus(const RendererSharedState& state);

// Runs `action` with the driven car back at its start transform, and returns it to the road after:
// a scene saved while driving records where the car was placed, not where it was driven to.
void RunWithVehicleAtStart(RendererSharedState& state, const std::function<void()>& action);

// Keyboard: W/S or the arrow keys for throttle and reverse, A/D or left/right to steer, Space for the
// hand brake. Gamepad: right and left trigger, left stick, South button (A on Xbox).
// `keyboardSteering` is the eased keyboard steering, carried between frames.
VehicleControls ReadVehicleControls(const InputState& input, bool keyboardCaptured, float deltaSeconds, float& keyboardSteering);

// Adds the static collision for every loaded model except `exclude`: its opaque and alpha-tested
// submeshes' triangles in world space. Returns the lowest vertex height, or `fallbackFloor` when
// nothing was added.
float AddSceneCollision(PhysicsWorld& physics, const RendererWorld& renderWorld, const ISceneWorld& scene, entt::entity exclude, float fallbackFloor);

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

// Turns the orbit by a mouse movement (degrees), or, with `lookHeld` false, eases it back to zero.
// Pitch is kept between looking a little up from below and straight down.
void UpdateCameraOrbit(VehicleCameraOrbit& orbit, bool lookHeld, float yawDeltaDegrees, float pitchDeltaDegrees, const VehicleCameraSettings& settings, float deltaSeconds);

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
