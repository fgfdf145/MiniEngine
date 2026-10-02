#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

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

// One axle's tyres as the physics engine models them: a friction that rises with slip to a peak and
// falls a little past it, along the wheel (slip ratio) and across it (slip angle). Each 0 keeps the
// engine's own default.
struct VehicleTyreSettings
{
    // The friction coefficient at the peak; the surface's friction multiplies it.
    float longitudinalGrip = 0.0f;
    float lateralGrip = 0.0f;
    // Where the peaks are: the slip ratio (wheel speed over ground speed less one) and the slip angle.
    float peakSlipRatio = 0.0f;
    float peakSlipAngleDegrees = 0.0f;
    // The share of the peak grip left once the tyre is well past it (0 keeps the physics engine's
    // five sixths).
    float postPeakShare = 0.0f;
    // The wheel's moment of inertia with its tyre, kg m^2.
    float inertia = 0.0f;

    bool operator==(const VehicleTyreSettings&) const = default;
};

// One axle's suspension linkage and its springs as a car's data gives them, for the multibody
// suspension (engine/suspension) that then replaces the physics engine's straight spring: the
// wheel follows its hardpoints' kinematics (camber, toe, steering, track change) and the spring,
// damper, bump stops and anti-roll bar act through them.
//
// Hardpoints are the left wheel's, from the wheel centre at the design position, in the axes
// (forward, outward, up), metres. The right wheel is the mirror image. Rates are at the wheel (per
// metre of wheel-centre travel), as Assetto Corsa gives them.
enum class VehicleSuspensionType : uint32_t
{
    None = 0,
    DoubleWishbone = 1,
    MacPherson = 2
};

struct VehicleSuspensionAxle
{
    VehicleSuspensionType type = VehicleSuspensionType::None;
    glm::vec3 lowerFront{0.0f};
    glm::vec3 lowerRear{0.0f};
    glm::vec3 lowerBall{0.0f};
    glm::vec3 upperFront{0.0f}; // double wishbone
    glm::vec3 upperRear{0.0f};
    glm::vec3 upperBall{0.0f};
    glm::vec3 strutTop{0.0f};   // MacPherson: top mount on the body
    glm::vec3 strutLower{0.0f}; // and a point of the strut axis on the knuckle
    glm::vec3 tieInner{0.0f};   // tie rod (front) or toe link (rear) on the rack or body
    glm::vec3 tieOuter{0.0f};
    float staticCamberDegrees = 0.0f; // negative: the wheel's top leans in
    // Toe set by the tie rod's length (Assetto Corsa's TOE_OUT): metres the rod is made longer or
    // shorter, positive in whichever way turns the wheel's front outward.
    float toeOutRodLength = 0.0f;
    float track = 0.0f;

    float wheelRate = 0.0f;            // N/m
    float progressiveRate = 0.0f;      // N/m^2: the rate grows by this per metre of compression
    float bumpStopRate = 0.0f;         // N/m
    float bumpStopTravel = 0.0f;       // compression from the design position where it starts, m
    float reboundStopTravel = 0.0f;    // extension from the design position to full droop, m
    float dampBump = 0.0f;             // N s/m below the fast threshold
    float dampFastBump = 0.0f;
    float dampFastBumpThreshold = 0.0f; // m/s
    float dampRebound = 0.0f;
    float dampFastRebound = 0.0f;
    float dampFastReboundThreshold = 0.0f;
    float antiRollBarRate = 0.0f;      // N/m of left/right travel difference
    float hubMass = 0.0f;              // kg, unsprung, each side
    // The tyre as a vertical spring: unloaded radius, rate and damping (for rigs with unsprung masses).
    float tyreRadius = 0.0f;
    float tyreRate = 0.0f;             // N/m
    float tyreDamping = 0.0f;          // N s/m
    // The centre of mass's height above the wheel centre (Assetto Corsa's BASEY, negative below).
    float centerOfMassAboveWheel = 0.0f;
    // The damper's seal friction at the wheel (LuGre): Coulomb and breakaway force, N. No car data
    // gives these; 0 leaves it out.
    float frictionCoulomb = 0.0f;
    float frictionBreakaway = 0.0f;

    bool operator==(const VehicleSuspensionAxle&) const = default;
};

// Where the air pushes on a car: at `position`, from the centre of mass, with these effective areas.
struct VehicleAeroSurface
{
    glm::vec3 position{0.0f};
    float dragArea = 0.0f;
    float downforceArea = 0.0f;
};

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
    // How the car's own data places its mass (ApplyCarSpec); 0 leaves FitVehicleSettingsToBounds to
    // guess from the model's size. The front axle's share of the weight, the centre of mass's height
    // above the ground (m), and the box (width, height, length, m) whose uniform inertia the body takes
    // instead of the collision box's.
    float frontWeightShare = 0.0f;
    float centerOfMassHeight = 0.0f;
    glm::vec3 inertiaBox{0.0f};

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
    // The engine's torque by rpm, in Nm (its peak is maxEngineTorque); the default shape of the
    // physics engine's when empty. Sorted by rpm.
    std::vector<glm::vec2> torqueCurve;
    // The gearbox: forward ratios first gear up, the reverse ratio (negative), and the final drive
    // ratio; the physics engine's own five-speed when empty. The shift points are in rpm, or the
    // engine's when 0.
    std::vector<float> gearRatios;
    float reverseGearRatio = 0.0f;
    float finalDriveRatio = 0.0f;
    float shiftUpRpm = 0.0f;
    float shiftDownRpm = 0.0f;
    // How long a gear change takes with no torque (s), and how long the clutch then takes to bite;
    // the physics engine's half a second and 0.3 s when 0. A dual-clutch box changes in a few
    // hundredths.
    float gearSwitchSeconds = 0.0f;
    float clutchReleaseSeconds = 0.0f;
    // The engine's moment of inertia, kg m^2 (the physics engine's 0.5 when 0). It steals torque from
    // the wheels while the revs climb, most in the low gears.
    float engineInertia = 0.0f;
    // How hard the clutch drags the wheels along with the engine, torque per rad/s of difference (the
    // physics engine's 10 when 0).
    float clutchStrength = 0.0f;
    // The tyres: with any grip set the vehicle's tyres multiply the surface's friction (as the game does)
    // instead of the physics engine's square root of the two.
    VehicleTyreSettings frontTyres;
    VehicleTyreSettings rearTyres;
    // The air: the surfaces it acts on, each where it sits from the centre of mass (vehicle axes), as
    // the drag and the downforce it makes per unit of dynamic pressure (coefficient times area, m^2).
    std::vector<VehicleAeroSurface> aeroSurfaces;
    // The brakes' torque per wheel averaged over the four, of which the front axle takes this
    // share (0.5 is an even split). A torque of 0 is automatic: what the car's weight and tyres can
    // hold without the wheels locking, see ComputeBrakeTorquePerWheel. Braking throws weight onto the
    // front axle, so it takes the larger share, though not much more than 0.55 before the fronts lock.
    float maxBrakeTorque = 0.0f; // Nm per wheel
    float frontBrakeShare = 0.55f;
    // With this on, the four wheels share the brakes' total torque (four times maxBrakeTorque) by the
    // load each carries at the moment: braking, cornering and bumps move weight between them, and a
    // wheel in the air gets none. frontBrakeShare then only holds until the car's loads are known.
    bool dynamicBrakeBias = true;
    float maxHandBrakeTorque = 4000.0f; // Nm per rear wheel
    VehicleDrive drive = VehicleDrive::RearWheel;
    bool antiRollBars = true;
    bool limitedSlipDifferentials = true;
    // How much of the drive torque a limited-slip differential can move from the wheel that spins to the one
    // that grips: 0 is open, 1 nearly locked.
    float limitedSlipLock = 0.4f;
    // Traction control: the clutch slips once the engine asks the driven wheels for more torque than their
    // tyres can hold, this share of their peak grip on the load they carry (1 is the limit, less stays short of
    // it). 0 is off. Without it a car at full throttle in a low gear spins its tyres several times over.
    float tractionControlGrip = 0.85f;
    // The body's linear damping, a fraction of its speed lost each second: the physics engine's 0.05, a
    // stand-in for air drag that a car with its own aerodynamics sets to 0.
    float linearDamping = 0.05f;
    // Past this pitch or roll the constraint stops tilting the car further; 180 leaves it free.
    float maxPitchRollDegrees = 60.0f;

    // The multibody suspension, used when both axles have a type: the hardpoints then decide where
    // each wheel goes and how it leans, the rates replace suspensionFrequencyHz / suspensionDamping
    // and antiRollBars, and steering moves the rack (the wheels' angles follow from the linkage;
    // steeringRackTravel is the rack's travel at full lock, 0 to fit it to maxSteerAngleDegrees).
    VehicleSuspensionAxle frontSuspension;
    VehicleSuspensionAxle rearSuspension;
    float steeringRackTravel = 0.0f;

    // Read by whoever starts a car, not by the physics: a model that carries its own figures
    // (VehicleCarSpec) drives on them instead of the fields above they cover.
    bool useCarData = true;
};

// One axle of one tyre compound as the game's data keeps it, whole: every number of its sections by
// upper-case key ("DX0", "FZ0", "FRICTION_LIMIT_ANGLE"; the thermal section's under "THERMAL_"), and
// the curves its files hold by the key that names them ("WEAR_CURVE", "PERFORMANCE_CURVE"). The
// physics engine's tyres use only what VehicleCarSpec::frontTyres and rearTyres pick from it.
struct VehicleTyreData
{
    std::string name;
    std::string shortName;
    std::map<std::string, float> values;
    std::map<std::string, std::vector<glm::vec2>> curves;
};

struct VehicleTyreCompound
{
    VehicleTyreData front;
    VehicleTyreData rear;
};

// A wing (or the body, or the underfloor) of the car's aerodynamics: a chord and span whose area the
// air's dynamic pressure acts on through coefficients that depend on the angle of attack.
struct VehicleAeroWing
{
    std::string name;
    float chord = 1.0f;
    float span = 1.0f;
    // From the centre of mass, in the game's axes.
    glm::vec3 position{0.0f};
    float angleDegrees = 0.0f;
    float liftGain = 1.0f;
    float dragGain = 1.0f;
    // Angle of attack in degrees to coefficient. Lift is negative for downforce.
    std::vector<glm::vec2> liftCurve;
    std::vector<glm::vec2> dragCurve;
    // The wing's other curves (the ground-height ones), by the key that names them.
    std::map<std::string, std::vector<glm::vec2>> curves;
    // The zone modifiers (ZONE_FRONT_CL and the like) and anything else numeric, by key.
    std::map<std::string, float> values;
};

// A wing whose angle follows an input of the car, such as a rear spoiler that rises with speed.
struct VehicleAeroController
{
    int wing = 0;
    std::string input;
    std::string combinator;
    std::vector<glm::vec2> curve;
    float filter = 0.0f;
    float upLimit = 0.0f;
    float downLimit = 0.0f;
};

// A turbocharger: steady boost is `min(maxBoost, wastegate)` reached by (rpm / referenceRpm)^gamma.
struct VehicleTurbo
{
    float maxBoost = 0.0f;
    float wastegate = 0.0f;
    float referenceRpm = 0.0f;
    float gamma = 1.0f;
    float lagUp = 0.0f;
    float lagDown = 0.0f;
};

// What a car's own data says, in SI units, for the fields it knows (Assetto Corsa's data.acd, read by
// the kn5 import). Whatever it leaves out stays as the tuning has it.
struct VehicleCarSpec
{
    std::optional<float> massKg;
    std::optional<VehicleDrive> drive;
    // The engine's torque at the crank by rpm, boost included: its peak is the engine's torque.
    std::vector<glm::vec2> torqueCurve;
    std::optional<float> minRpm;
    std::optional<float> maxRpm;
    std::vector<float> gearRatios;
    std::optional<float> reverseGearRatio;
    std::optional<float> finalDriveRatio;
    std::optional<float> gearSwitchSeconds;
    std::optional<float> clutchReleaseSeconds;
    std::optional<float> engineInertia;
    // The tyres the car starts on, as the physics engine takes them (see tyreCompounds).
    std::optional<VehicleTyreSettings> frontTyres;
    std::optional<VehicleTyreSettings> rearTyres;
    // The air: the car's wings with their curves, and the controllers that move them. Drag and
    // downforce use the wings at their base angle.
    std::vector<VehicleAeroWing> aeroWings;
    std::vector<VehicleAeroController> aeroControllers;

    // Data the physics engine has no place for yet, kept whole so a later feature does not need the
    // car imported again.
    // Every tyre compound the car ships and the index of the one it starts on.
    std::vector<VehicleTyreCompound> tyreCompounds;
    std::optional<int> defaultTyreCompound;
    std::vector<VehicleTurbo> turbos;
    // Engine braking at a reference rpm, with the throttle closed.
    std::optional<float> coastRpm;
    std::optional<float> coastTorque;
    // The gearbox: how long a change takes each way and how long the ignition is cut on an upshift
    // (seconds), the clutch's torque limit (Nm) and the autoclutch's engagement window (rpm) and
    // profiles (the seconds of each point).
    std::optional<float> changeUpSeconds;
    std::optional<float> changeDownSeconds;
    std::optional<float> autoCutoffSeconds;
    std::optional<float> clutchMaxTorque;
    std::optional<float> autoClutchMinRpm;
    std::optional<float> autoClutchMaxRpm;
    std::vector<float> upshiftClutchProfile;
    std::vector<float> downshiftClutchProfile;
    // The differential's lock under power and on the overrun (0 to 1) and its preload (Nm).
    std::optional<float> differentialPower;
    std::optional<float> differentialCoast;
    std::optional<float> differentialPreload;
    // The driver aids: section (ABS, TRACTION_CONTROL, EDL) to its numbers (PRESENT, ACTIVE,
    // SLIP_RATIO_LIMIT, MIN_SPEED_KMH, RATE_HZ, ...).
    std::map<std::string, std::map<std::string, float>> electronics;
    // The front wheels' lock each way, and how far the steering wheel turns each way to reach it.
    std::optional<float> maxSteerAngleDegrees;
    std::optional<float> steeringWheelLockDegrees;
    std::optional<float> brakeTorquePerWheel;
    std::optional<float> frontBrakeShare;
    std::optional<float> handBrakeTorquePerWheel;
    std::optional<float> suspensionFrequencyHz;
    // As a fraction of critical damping.
    std::optional<float> suspensionDamping;
    std::optional<bool> antiRollBars;
    std::optional<bool> limitedSlipDifferentials;
    // The suspension linkage and its rates, per axle.
    std::optional<VehicleSuspensionAxle> frontSuspension;
    std::optional<VehicleSuspensionAxle> rearSuspension;
    // Where the axles are and how the mass sits: the wheelbase (m), the front axle's share of the
    // weight, and the box (width, height, length, m) the body's inertia is a uniform box of.
    std::optional<float> wheelbase;
    std::optional<float> frontWeightShare;
    std::optional<glm::vec3> inertiaBox;
};

// Whether the settings carry a multibody suspension for both axles.
bool HasSuspensionGeometry(const VehicleSettings& settings);

// `tuning` with the fields `spec` knows replaced by its figures.
VehicleSettings ApplyCarSpec(const VehicleSettings& tuning, const VehicleCarSpec& spec);

// The brake torque per wheel (Nm) that stops the car as hard as its tyres allow, a little short of
// locking them: 75% of the grip on the car's weight (66% with dynamicBrakeBias), over the wheel radius,
// shared by four wheels.
// The tuned maxBrakeTorque when it is set.
float ComputeBrakeTorquePerWheel(const VehicleSettings& settings);

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
