#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace me
{

// Vehicle space: +Y up and +Z forward, so +X is the car's left. Lengths are metres, after the
// entity's scale. A model whose front faces -Z (VehicleSettings::modelFront) is turned half a turn
// about Y into it by whoever places the car.

// Which way the car's model faces in its own space.
enum class VehicleModelFront : uint32_t
{
    // Assetto Corsa cars as the kn5 import writes them (+X right).
    NegativeZ = 0,
    // glTF's "front of the asset" (+X left).
    PositiveZ = 1
};

enum class VehicleDrive : uint32_t
{
    RearWheel = 0,
    FrontWheel = 1,
    AllWheel = 2
};

// One wheel: its centre in vehicle space and its tyre's size. In a VehicleWheelLayout the centre is
// where the model draws the wheel at rest.
struct VehicleWheelGeometry
{
    glm::vec3 center{0.0f};
    float radius = 0.32f;
    float width = 0.22f;
};

// The wheels' order everywhere: front left, front right, rear left, rear right.
inline constexpr size_t kVehicleWheelCount = 4;
using VehicleWheelLayout = std::array<VehicleWheelGeometry, kVehicleWheelCount>;

struct VehicleSettings
{
    float massKg = 1400.0f;
    VehicleModelFront modelFront = VehicleModelFront::NegativeZ;

    // The collision box, in vehicle space. It starts above the wheels' lowest point, so the chassis
    // only touches the ground when the suspension bottoms out.
    glm::vec3 chassisCenter{0.0f, 0.85f, 0.0f};
    glm::vec3 chassisHalfExtents{0.9f, 0.5f, 2.2f};
    // The centre of mass relative to chassisCenter. A car's sits low; a box's centre would roll it
    // over in the first corner.
    glm::vec3 centerOfMassOffset{0.0f, -0.35f, 0.0f};

    // The wheels: front left, front right, rear left, rear right, placed by the axles' Z, the half
    // track width either side of trackCenterX, and the suspension's top mount at wheelMountY.
    float wheelRadius = 0.32f;
    float wheelWidth = 0.22f;
    float frontAxleZ = 1.35f;
    float rearAxleZ = -1.35f;
    float trackCenterX = 0.0f;
    float halfTrackWidth = 0.78f;
    float wheelMountY = 0.5f;
    // Where the model puts each wheel, when it does (a kn5's WHEEL_xx nodes). Then these replace the
    // axles, track and radius above: each wheel hangs from center plus the rest suspension
    // length, so the model's wheels sit where they were drawn once the springs settle.
    bool hasWheelLayout = false;
    VehicleWheelLayout wheelLayout{};

    // Spring travel below the mount, in metres, and its stiffness as a natural frequency: 1-2 Hz is a
    // road car, 3+ a race car.
    float suspensionMinLength = 0.05f;
    float suspensionMaxLength = 0.3f;
    float suspensionFrequencyHz = 1.5f;
    float suspensionDamping = 0.5f;

    float maxSteerAngleDegrees = 35.0f;
    float maxEngineTorque = 500.0f; // Nm
    float minRpm = 1000.0f;
    float maxRpm = 7000.0f;
    float maxBrakeTorque = 1500.0f;     // Nm per wheel
    float maxHandBrakeTorque = 4000.0f; // Nm per rear wheel
    VehicleDrive drive = VehicleDrive::RearWheel;
    bool antiRollBars = true;
    bool limitedSlipDifferentials = true;
    // Past this pitch or roll the constraint stops tilting the car further; 180 leaves it free.
    float maxPitchRollDegrees = 60.0f;
};

// `tuning` with its geometry fitted to a car whose model spans these vehicle-space bounds (the
// model's bounds times the entity's scale): the wheels at its corners, the chassis box over them, the
// suspension travel, and a ride height that puts the tyres' contact patch on the bounds' floor once the
// springs have settled under the car's weight. Mass, engine, brakes, steering, drive and spring rate
// are kept from `tuning`.
// With `wheelLayout`, the wheels are where the model has them instead of guessed from the bounds
// (the axles and track are then their averages, the suspension travel follows the tyres' radius).
VehicleSettings FitVehicleSettingsToBounds(
    const glm::vec3& minBounds,
    const glm::vec3& maxBounds,
    const VehicleSettings& tuning = {},
    const VehicleWheelLayout* wheelLayout = nullptr);

// The wheel `index` (front left, front right, rear left, rear right) as the vehicle is built with
// it: its top mount, from which the suspension hangs down, its radius and its width.
VehicleWheelGeometry GetVehicleWheelMount(const VehicleSettings& settings, size_t index);

// How far the suspension hangs below its mount once the car rests on flat ground under gravity
// (metres per second squared), with the weight spread evenly over the four wheels. Clamped to the
// suspension's travel.
float ComputeRestSuspensionLength(const VehicleSettings& settings, float gravity);

// What the driver asks for, from the keyboard or a gamepad.
struct VehicleControls
{
    float throttle = 0.0f; // -1 full reverse, 1 full forward
    float steering = 0.0f; // -1 full left, 1 full right
    float brake = 0.0f;    // 0-1
    float handBrake = 0.0f; // 0-1
};

// The inputs of Jolt's WheeledVehicleController::SetDriverInput.
struct VehicleDriverInput
{
    float forward = 0.0f;
    float right = 0.0f;
    float brake = 0.0f;
    float handBrake = 0.0f;
};

// Turns controls into driver input the way a car with an automatic gearbox drives: throttle against
// the direction of travel brakes until the car has stopped, and only then does the gearbox switch
// between drive and reverse. `direction` is that gearbox state, +1 or -1, kept by the caller between
// calls; `forwardSpeed` is the car's velocity along its forward axis in metres per second. The hand
// brake cuts the throttle.
VehicleDriverInput ResolveVehicleDriverInput(const VehicleControls& controls, float forwardSpeed, float& direction);
}
