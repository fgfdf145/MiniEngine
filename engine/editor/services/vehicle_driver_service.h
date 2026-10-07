#pragma once

#include <engine/asset/model_driver_pose.h>
#include <engine/asset/model_loader.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace me
{

struct RendererSharedState;

// Where a driver sits in a car, in the car's vehicle space (+Z forward, +X the car's left, +Y up,
// from the model's origin, in the model's units), as FitDriverSeat finds it.
struct VehicleDriverSeat
{
    glm::vec3 hips{0.0f};
    float reclineDegrees = 15.0f;
    // How far the back bends forward off the seat's (DriverPoseInput::leanDegrees).
    float leanDegrees = 0.0f;
    glm::vec3 wheelCenter{0.0f};
    // The steering column, pointing away from the driver.
    glm::vec3 wheelAxis{0.0f, 0.0f, 1.0f};
    // The rim's middle line about the column, and the rim's own radius (half its thickness).
    float wheelRadius = 0.18f;
    float wheelTubeRadius = 0.015f;
    // +1 when wheelAxis is the model's own steering axis (ModelSteeringWheel::axis), -1 when it was
    // turned round to point away from the driver: the wheel's turn about it has this sign.
    float wheelTurnSign = 1.0f;
    // Where the balls of the feet press the pedals (vehicle space, on their faces): found across the
    // footwell, else put where a car's usually are.
    glm::vec3 throttle{0.0f};
    glm::vec3 brake{0.0f};
    glm::vec3 clutch{0.0f};
    glm::vec3 footRest{0.0f};
    bool pedalsFound = false;
    // The driver's legs: from the hip to the ankle, the hips' half width, and the line from the ankle
    // to the ball of the foot (its length and how far the rest pose has it below level, radians).
    float legLength = 0.87f;
    float hipHalfWidth = 0.07f;
    float footLength = 0.13f;
    float footRestPitch = -0.4f;
    // The seat's cushion (its height) and back (a point on it, at the hips' height, and its facing),
    // which the driver's hair and skirt lie on.
    float cushionHeight = 0.4f;
    glm::vec3 backPoint{0.0f};
    glm::vec3 backNormal{0.0f, 0.0f, 1.0f};
    // The driver's eyes the seat was found from: the car's data, else from the steering wheel.
    glm::vec3 carEyes{0.0f};
    // How far forward the hips slid for the hands to reach the wheel (metres).
    float slide = 0.0f;
};

// Fits a humanoid driver (its skeleton and rig) to the car's driver's seat: the seat's cushion and
// back, the floor and the toe board are found by casting rays through the car's meshes round its
// driver's eyes (the car's data, else 0.59 m behind and 0.29 m over its steering wheel), and the hips
// slide forward until the hands reach the wheel with the elbows bent. Nullopt for a car without a
// steering wheel.
std::optional<VehicleDriverSeat> FitDriverSeat(
    const LoadedModelData& car,
    const glm::quat& vehicleToModel,
    const ModelSkeleton& skeleton,
    const DriverRig& rig);

// Where the driver's feet are on the pedals: the right foot from the accelerator (0) to the brake (1),
// the left from the foot rest (0) to the clutch (1), and how far each pedal is pressed (0 to 1).
struct VehicleDriverFeet
{
    float rightOnBrake = 0.0f;
    float leftOnClutch = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    float clutch = 0.0f;
    // How long the left foot stays on the clutch after it was last pressed (seconds).
    float clutchHold = 0.0f;
};

// The body thrown about by the car: a damped spring on its lean, driven by what the driver feels where
// it sits (the acceleration of that point of the car's body, which rolls and pitches on its springs,
// less gravity, in the body's own frame), forward under braking, back a little under power, outwards in
// a corner and as the body rolls.
struct VehicleDriverSway
{
    int samples = 0;
    glm::vec3 lastPosition{0.0f};
    glm::vec3 lastVelocity{0.0f};
    // What the driver feels, smoothed, in vehicle space (m/s^2).
    glm::vec3 felt{0.0f};
    // To the driver's left and forward (degrees, DriverPoseInput::swayDegrees), and how fast they change.
    glm::vec2 lean{0.0f};
    glm::vec2 leanRate{0.0f};
};

// Moves the sway on for the car's vehicle space at vehicleToWorld now, felt at `feltAt` (a point of the
// car in vehicle space); deltaSeconds of 0 holds it.
void UpdateDriverSway(VehicleDriverSway& sway, const glm::mat4& vehicleToWorld, const glm::vec3& feltAt, float deltaSeconds);

// Where the hands hold the steering wheel, hand over hand: each holds a point of the rim and turns with
// it until it is as far round as it reaches (the left hand from 170 degrees left of the top to 100
// right of it, the right the other way round), then lets go and takes hold again further back, crossing
// over the other arm; back near the middle each hand goes back to a quarter to three. One hand at a time.
struct VehicleDriverHands
{
    bool started = false;
    float lastTurn = 0.0f;
    // Where each hand holds the rim, as the wheel's own (DriverPoseInput::gripAngles less its turn).
    std::array<float, 2> grips{};
    // A hand on its way to a new hold: how long it has been going, and where it let go (vehicle space).
    std::array<bool, 2> moving{};
    std::array<float, 2> moveSeconds{};
    std::array<glm::vec3, 2> releasedAt{};
};

// Moves the hands on for the wheel turned `turn` (radians about the seat's wheelAxis, clockwise as the
// driver sees it).
void UpdateDriverHands(VehicleDriverHands& hands, const VehicleDriverSeat& seat, float turn, float deltaSeconds);

// The pedals as the driver works them: from the car's controls (the accelerator and the brake, the
// throttle pulled against the gear braking) and the clutch's opening (the pedal, or the automatic
// clutch while the gears change). Moves the feet between pedals over a moment and eases the presses.
void UpdateDriverFeet(VehicleDriverFeet& feet, float throttle, float brake, float clutch, float deltaSeconds);

// What moves the driver from frame to frame.
struct VehicleDriverMotion
{
    VehicleDriverFeet feet;
    VehicleDriverSway sway;
    VehicleDriverHands hands;
};

// The driver's pose in the seat, moved by `seatOffset` (metres: to the car's right, up, forward),
// holding the wheel turned `steeringWheelTurn` (radians about ModelSteeringWheel::axis, as
// VehicleDriveSession::steeringWheelTurn) where `motion` has the hands (else at a quarter to three, as
// at rest), its feet and body as it has them. The driver's model space is the car's vehicle space.
DriverPoseInput DriverPoseFromSeat(
    const VehicleDriverSeat& seat,
    const glm::vec3& seatOffset,
    float steeringWheelTurn,
    bool hideHead,
    const VehicleDriverMotion* motion = nullptr);

struct VehicleDriverState
{
    struct FittedSeat
    {
        // Keep the models alive, so the key's addresses are never another model's.
        std::shared_ptr<const LoadedModelData> car;
        std::shared_ptr<const LoadedModelData> driver;
        std::optional<VehicleDriverSeat> seat;
    };
    // By car model and driver model.
    std::map<std::pair<const LoadedModelData*, const LoadedModelData*>, FittedSeat> seats;
    // The entities posed as drivers in the last frame.
    std::unordered_set<entt::entity> posed;
    // Each driver's feet, body and hands, carried from frame to frame.
    std::unordered_map<entt::entity, VehicleDriverMotion> motion;
    // Entity ids found before, checked again on use.
    std::unordered_map<std::string, entt::entity> entitiesById;
    // Why a model with a car to drive is not sitting in it; no entry when it is.
    std::unordered_map<entt::entity, std::string> problems;
};

namespace VehicleDriverService
{
// Per frame, after the drive and before the animations: every model seated in a car
// (ModelComponent::driverVehicleUuid) takes the car's transform, its model space the car's vehicle
// space, and a humanoid one sits in the driver's seat holding the steering wheel as the drive turns
// it. While the car is driven from the cockpit, the driver's head is hidden and the camera sits at its
// eyes; its feet work the pedals, a hand changes gear on the gear lever, the hands go hand over hand
// round the wheel and the body sways with the car.
void Tick(RendererSharedState& state, float deltaSeconds);

// Why the entity is not sitting in its car (no such car, no steering wheel, no humanoid skeleton);
// empty when it is, or when it drives none.
std::string Problem(const RendererSharedState& state, entt::entity entity);
}
}
