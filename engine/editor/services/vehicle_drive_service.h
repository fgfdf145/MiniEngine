#pragma once

#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>
#include <engine/renderer/camera.h>
#include <engine/scene/scene_components.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <functional>
#include <memory>
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

// A model being driven as a car: the physics world built for it, and what to put back when it stops.
struct VehicleDriveSession
{
    entt::entity entity = entt::null;
    std::string name;
    TransformComponent startTransform;
    PhysicsPose startPose;
    glm::vec3 scale{1.0f};
    Camera cameraBeforeDriving;
    std::unique_ptr<PhysicsWorld> physics;
    VehicleId vehicle = 0;
    bool paused = false;
    bool stepRequested = false;
    // The keyboard's steering, eased towards full lock rather than jumping to it.
    float keyboardSteering = 0.0f;
    bool resetHeld = false;
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

// Eases the camera towards its place behind the car, looking at it. With deltaSeconds of zero it
// jumps straight there.
void UpdateChaseCamera(Camera& camera, const PhysicsPose& vehiclePose, const VehicleCameraSettings& settings, float deltaSeconds);
}
}
