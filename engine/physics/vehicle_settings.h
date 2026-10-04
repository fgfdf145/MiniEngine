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

// What makes the tyres' grip.
enum class VehicleTyreModel : uint32_t
{
    // The physics engine's own: friction curves of slip that its constraint solver clamps the
    // contact's impulse to (VehicleTyreSettings shapes them).
    PhysicsEngine = 0,
    // A brush tyre on a flexible carcass (engine/tyre/tyre_brush.h, after Stocco, Biral & Bertolazzi
    // 2024), its parameters fitted to VehicleTyreSettings' grip and peak slip angle and to the wheel's
    // size and tyre rate. Its forces replace the physics engine's tyre friction.
    Brush = 1
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
    // The brush tyre's load sensitivity: its friction scales as (load / static load)^(loadExponent - 1),
    // so the peak force grows as the load to this power (0 keeps the brush tyre's 0.9).
    float loadExponent = 0.0f;

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
    MacPherson = 2,
    SolidAxle = 3 // a live axle on links (Assetto Corsa's AXLE): both wheels on one beam
};

// A link of a solid axle: its chassis end and its end on the axle, in the axle's frame (forward, left,
// up from the axle's centre, midway between the wheel centres, at the design position).
struct VehicleAxleLink
{
    glm::vec3 chassis{0.0f};
    glm::vec3 axle{0.0f};
    bool operator==(const VehicleAxleLink&) const = default;
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
    // The centre of mass's height above the wheel centre (Assetto Corsa's BASEY negated: BASEY is the
    // wheel centre's height against the centre of mass).
    float centerOfMassAboveWheel = 0.0f;
    // The damper's seal friction at the wheel (LuGre): Coulomb and breakaway force, N. No car data
    // gives these; 0 leaves it out.
    float frictionCoulomb = 0.0f;
    float frictionBreakaway = 0.0f;

    // A solid axle (type SolidAxle; the hardpoints above are then unused): its links, where its springs
    // and dampers sit along each half (0 at the centre, 1 at the wheel; their rates are there), what
    // else holds it sideways (leaf springs, N/m), and Assetto Corsa's TORQUE_REACTION (kept, not
    // modelled).
    std::vector<VehicleAxleLink> axleLinks;
    float axleSpringPosition = 1.0f;
    float axleLateralStiffness = 0.0f;
    float axleTorqueReaction = 0.0f;

    bool operator==(const VehicleSuspensionAxle&) const = default;
};

// Where the air pushes on a car: at `position`, from the centre of mass, with these effective areas.
struct VehicleAeroSurface
{
    glm::vec3 position{0.0f};
    float dragArea = 0.0f;
    float downforceArea = 0.0f;
};

// One of Assetto Corsa's controllers ([CONTROLLER_n] of ctrl_4ws.ini, ctrl_awd2.ini, ctrl_ers_N.ini):
// a telemetry channel (STEER_DEG, SPEED_KMH, GAS, GEAR, SLIPRATIO_MAX, ...) read through its curve,
// combined with the value so far (ADD or MULT), filtered and kept within its limits. See
// EvaluateVehicleControllers.
struct VehicleController
{
    std::string input;
    std::string combinator;
    std::vector<glm::vec2> curve;
    float filter = 0.0f;
    float upLimit = 0.0f;
    float downLimit = 0.0f;
};

// A box of a car's body (Assetto Corsa's colliders.ini [COLLIDER_n]): its centre from the centre of mass
// in the vehicle's frame (+X left, +Y up, +Z forward), its full size, and whether the game lets it meet
// the ground (kept; every box collides here).
struct VehicleColliderBox
{
    glm::vec3 center{0.0f};
    glm::vec3 size{0.0f};
    bool groundEnabled = true;

    bool operator==(const VehicleColliderBox&) const = default;
};

// How the engine's torque reaches the front axle of a four-wheel-drive car.
enum class VehicleCentreDrive
{
    // A centre differential: the front gets frontTorqueShare of the engine's torque.
    Differential,
    // Rear drive with a coupling at the transfer case (Assetto Corsa's AWD2, Nissan's ATTESA): the front gets
    // what the coupling passes, min(rampTorque * slip, maxTorque) on the shaft speeds' difference.
    Coupling,
};

// One axle's differential as a clutch-pack limited slip: the share of the torque through it that locks
// it under power (below 0: the car's limitedSlipLock) and on the overrun, the engine braking (below 0:
// the car's limitedSlipCoast), and its preload (Nm, below 0: the car's limitedSlipPreload).
struct VehicleAxleDifferential
{
    float lock = -1.0f;
    float preload = -1.0f;
    float coast = -1.0f;
};

// How a hybrid's motor torque (VehicleSettings::ersTorqueCurve) reaches the drivetrain.
enum class VehicleErsDelivery
{
    None,
    // Folded into torqueCurve at full deployment: the battery never runs out and the deployment
    // profiles are ignored. The only mode so far; a run-time ERS (battery, profiles, recovery) would
    // keep torqueCurve the engine's and add ersTorqueCurve, scaled, every step.
    AddedToEngine,
};

// A turbocharger: steady boost is maxBoost scaled by min(1, (throttle * rpm / referenceRpm)^gamma),
// never past the wastegate (when it has one); several turbos' boosts add, and the torque is
// the curve's times one plus the boost (VehicleTurboBoost). The boost follows its steady level as the
// game's does, keeping lagUp (rising) or lagDown (falling) of its distance from it each of the game's
// 333 Hz steps; 0 follows at once.
struct VehicleTurbo
{
    float maxBoost = 0.0f;
    float wastegate = 0.0f;
    float referenceRpm = 0.0f;
    float gamma = 1.0f;
    float lagUp = 0.0f;
    float lagDown = 0.0f;
};

// A turbo's steady boost at these revs and this much throttle (0 to 1).
float VehicleTurboBoost(const VehicleTurbo& turbo, float rpm, float throttle);

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
    // The car's own body, replacing the box when there are carColliders: the boxes (from the centre of
    // mass, vehicle axes) and the shell's points in vehicle space (Assetto Corsa's collider.kn5, set by
    // whoever knows the model's frame), raised to the boxes' tops so that only the boxes meet the
    // ground. See BuildChassisParts.
    std::vector<VehicleColliderBox> carColliders;
    std::vector<glm::vec3> chassisHull;
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
    // The steering wheel's turn at full lock (degrees), for controllers reading STEER_DEG; 0: 450.
    float steeringWheelLockDegrees = 0.0f;
    // Rear-wheel steering (Assetto Corsa's ctrl_4ws.ini): the controllers give the rear wheels' angle in
    // radians, opposite to the front when it has the steering's sign (see EvaluateVehicleControllers and
    // ComputeRearSteerAngle). Empty: the rear wheels do not steer.
    std::vector<VehicleController> rearSteerControllers;
    float maxEngineTorque = 500.0f; // Nm
    float minRpm = 1000.0f;
    float maxRpm = 7000.0f;
    // The engine's torque by rpm, in Nm (its peak is maxEngineTorque); the default shape of the
    // physics engine's when empty. Sorted by rpm. With ersDelivery AddedToEngine it holds the ERS
    // motor's torque too.
    std::vector<glm::vec2> torqueCurve;
    // The turbos, whose full steady boost torqueCurve already has: at run time their boost builds and
    // falls with lag, and the engine makes the curve's torque scaled by (1 + boost) / (1 + full boost).
    std::vector<VehicleTurbo> turbos;
    // A hybrid's ERS motor torque at full deployment by rpm (Nm at the crank; empty: none), sorted, and
    // how it is delivered.
    std::vector<glm::vec2> ersTorqueCurve;
    VehicleErsDelivery ersDelivery = VehicleErsDelivery::None;
    // The gearbox: forward ratios first gear up, the reverse ratio (negative), and the final drive
    // ratio; the physics engine's own five-speed when empty. The automatic gearbox changes up at
    // shiftUpRpm on full throttle and down at shiftDownRpm on a closed one, in between by the
    // throttle (see ComputeVehicleShiftPoints); both come from the engine's rev range when 0.
    std::vector<float> gearRatios;
    float reverseGearRatio = 0.0f;
    float finalDriveRatio = 0.0f;
    float shiftUpRpm = 0.0f;
    float shiftDownRpm = 0.0f;
    // How long a gear change takes with no torque (s, the physics engine's half a second when 0), and how
    // long the clutch then takes to bite (0 at once, the engine's revs already matched; the physics
    // engine's 0.3 s when below 0). A dual-clutch box changes in a few hundredths.
    float gearSwitchSeconds = 0.0f;
    float clutchReleaseSeconds = -1.0f;
    // A change down's own time (0: gearSwitchSeconds), and how long an upshift cuts the engine for
    // (the game's AUTO_CUTOFF_TIME; 0: only while the clutch is open).
    float gearSwitchDownSeconds = 0.0f;
    float upshiftCutSeconds = 0.0f;
    // Moving off on the throttle the clutch slips to hold the engine near this rpm (full throttle; less
    // throttle, proportionally nearer the idle) until the car catches up with it, as a driver launching
    // does; 0 lets it bite over clutchReleaseSeconds from the idle. See UpdateAutomaticGearbox.
    float launchRpm = 4000.0f;
    // The engine's moment of inertia, kg m^2 (the physics engine's 0.5 when 0). It steals torque from
    // the wheels while the revs climb, most in the low gears.
    float engineInertia = 0.0f;
    // Engine braking: the torque the closed-throttle engine drags with at engineCoastRpm, taken as
    // proportional to the rpm and to the throttle left closed (Assetto Corsa's COAST_REF). 0 leaves
    // the physics engine's own drag of 0.2 of its speed per second.
    float engineCoastTorque = 0.0f;
    float engineCoastRpm = 0.0f;
    // How hard the clutch drags the wheels along with the engine, torque per rad/s of difference (the
    // physics engine's 10 when 0).
    float clutchStrength = 0.0f;
    // The tyres: with any grip set the vehicle's tyres multiply the surface's friction (as the game does)
    // instead of the physics engine's square root of the two.
    VehicleTyreSettings frontTyres;
    VehicleTyreSettings rearTyres;
    VehicleTyreModel tyreModel = VehicleTyreModel::PhysicsEngine;
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
    // Anti-lock brakes (Assetto Corsa's electronics.ini [ABS]): at absRateHz the controller looks at each
    // wheel and lets its brake off while it turns slower than the road by more than absSlipRatioLimit,
    // on again once it is back under. A limit of 0 is a car without ABS; useAbs off ignores the car's.
    bool useAbs = true;
    float absSlipRatioLimit = 0.0f;
    float absRateHz = 0.0f;
    // The game's traction control (electronics.ini [TRACTION_CONTROL]): above tcMinSpeedKmh, at tcRateHz,
    // the throttle is cut while a driven wheel turns faster than the road by more than tcSlipRatioLimit.
    // A car whose data has electronics uses this instead of tractionControlGrip (which it sets to 0);
    // a limit of 0 is none. useTractionControl off ignores the car's.
    bool useTractionControl = true;
    float tcSlipRatioLimit = 0.0f;
    float tcMinSpeedKmh = 0.0f;
    float tcRateHz = 0.0f;
    // The clutch's torque limit at the engine (Nm; 0 none): the physics engine's clutch is viscous and
    // would pass any torque its speed gap asks.
    float clutchMaxTorque = 0.0f;
    float maxHandBrakeTorque = 4000.0f; // Nm per rear wheel
    VehicleDrive drive = VehicleDrive::RearWheel;
    bool antiRollBars = true;
    bool limitedSlipDifferentials = true;
    // How much of the drive torque a limited-slip differential can move from the wheel that spins to the one
    // that grips: 0 is open, 1 nearly locked.
    float limitedSlipLock = 0.4f;
    // The same on the overrun, when the engine brakes the wheels (below 0: limitedSlipLock), and the
    // torque the clutch pack holds the wheels together with when nothing goes through it (Nm; below 0,
    // 40 Nm for a car without data).
    float limitedSlipCoast = -1.0f;
    float limitedSlipPreload = -1.0f;
    // Four-wheel drive (drive AllWheel): a centre differential sending frontTorqueShare to the front, or a
    // coupling at the transfer case passing min(rampTorque * shaft slip, maxTorque) (Nm per rad/s, Nm, at the
    // shaft) from the driven rear to the front. Each axle's differential (front, rear) may have its own
    // lock and preload.
    VehicleCentreDrive centreDrive = VehicleCentreDrive::Differential;
    float frontTorqueShare = 0.5f;
    float centreCouplingRampTorque = 0.0f;
    float centreCouplingMaxTorque = 0.0f;
    std::array<VehicleAxleDifferential, 2> axleDifferentials{};
    // Traction control: the clutch slips once the engine asks the driven wheels for more torque than their
    // tyres can hold, this share of their peak grip on the load they carry (1 is the limit, less stays short of
    // it). 0 is off. Without it a car at full throttle in a low gear spins its tyres several times over.
    float tractionControlGrip = 0.85f;
    // The body's linear damping, a fraction of its speed lost each second: the physics engine's 0.05, a
    // stand-in for air drag that a car with its own aerodynamics sets to 0.
    float linearDamping = 0.05f;
    // Past this pitch or roll the constraint stops tilting the car further; 180 leaves it free, so a
    // car tipped far enough rolls over.
    float maxPitchRollDegrees = 180.0f;

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


// A four-wheel drive's figures (Assetto Corsa's drivetrain.ini [AWD] for TYPE=AWD, [AWD2] for TYPE=AWD2):
// the front, centre and rear differentials' lock under power and on the overrun (0 to 1) and preload (Nm),
// a centre differential's front share, a centre coupling's ramp (Nm per rad/s) and limit (Nm), and the
// controllers that drive the centre (ctrl_awd2.ini, ctrl_awd_center_lock.ini: kept, not run).
struct VehicleAllWheelDrive
{
    bool coupling = false; // AWD2
    float frontShare = 0.5f;
    float frontDiffPower = 0.0f;
    float frontDiffCoast = 0.0f;
    float frontDiffPreload = 0.0f;
    float centreDiffPower = 0.0f;
    float centreDiffCoast = 0.0f;
    float centreDiffPreload = 0.0f;
    float rearDiffPower = 0.0f;
    float rearDiffCoast = 0.0f;
    float rearDiffPreload = 0.0f;
    float centreRampTorque = 0.0f;
    float centreMaxTorque = 0.0f;
    std::vector<VehicleController> centreControllers;
};

// A deployment profile the driver picks (ctrl_ers_N.ini): its name and its controllers in order.
struct VehicleErsProfile
{
    std::string name;
    std::vector<VehicleController> controllers;
};

// A hybrid's energy recovery system (Assetto Corsa's ers.ini): the kinetic motor's torque at full
// deployment and on the overrun, by rpm (Nm, added to the engine's at the crank), its battery and
// recovery figures, the heat recovery's, and the deployment profiles. Only torqueCurve is used yet
// (see VehicleSettings::ersDelivery); the rest is kept for a run-time ERS.
struct VehicleErs
{
    std::vector<glm::vec2> torqueCurve;
    std::vector<glm::vec2> coastCurve;
    float chargeK = 0.0f;          // [KINETIC] CHARGE_K: charge per brake torque and speed
    float dischargeSeconds = 0.0f; // a full battery's time at full deployment (DISCHARGE_TIME, ms)
    float maxKjPerLap = 0.0f;
    bool hasButtonOverride = false;
    float brakeRearCorrection = 0.0f;
    float heatChargeK = 0.0f;       // [HEAT] CHARGE_K: charge per turbo boost
    float heatTorquePercent = 0.0f; // [HEAT] TORQUE_PERC: what the heat recovery adds through the motor
    int defaultProfile = 0;
    std::vector<VehicleErsProfile> profiles;
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
    // A hybrid's ERS; torqueCurve above is the engine's alone. ApplyCarSpec adds the motor's torque.
    std::optional<VehicleErs> ers;
    // A four-wheel drive's differentials and centre (drive AllWheel), the rear-wheel steering's controllers,
    // and the body's collision boxes (from the centre of mass) and shell (collider.kn5's points in the
    // model's own frame).
    std::optional<VehicleAllWheelDrive> allWheelDrive;
    std::vector<VehicleController> rearSteerControllers;
    std::vector<VehicleColliderBox> colliders;
    std::vector<glm::vec3> colliderHull;
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
    // The game's automatic gearbox's change points ([AUTO_SHIFTER] UP, DOWN, rpm): UP is where ours changes
    // up on full throttle (ApplyCarSpec); DOWN is kept.
    std::optional<float> autoShiftUpRpm;
    std::optional<float> autoShiftDownRpm;
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
    // The fuel the car starts with (Assetto Corsa's [FUEL] FUEL, litres) and where its tank sits from
    // the centre of mass (m, [FUELTANK] POSITION: across, up, forward). massKg, frontWeightShare and
    // the axles' centerOfMassAboveWheel are the car without it (TOTALMASS has the driver, no fuel);
    // WithStartingFuel adds it.
    std::optional<float> fuelLitres;
    std::optional<glm::vec3> fuelTankPosition;
};

// Fuel's mass per litre. [Not checked against the game: petrol is 0.72 to 0.77.]
inline constexpr float kFuelKgPerLitre = 0.75f;

// `spec` with its starting fuel in the mass, the weight split and the centre of mass's height (the
// fuel fields then cleared, so applying it twice adds nothing). Unchanged without fuel or a mass.
VehicleCarSpec WithStartingFuel(const VehicleCarSpec& spec);

// Whether the settings carry a multibody suspension for both axles.
bool HasSuspensionGeometry(const VehicleSettings& settings);

// `tuning` with the fields `spec` knows replaced by its figures.
// A car with an ERS gets its motor's torque curve and, for now, ersDelivery AddedToEngine.
VehicleSettings ApplyCarSpec(const VehicleSettings& tuning, const VehicleCarSpec& spec);

// Two torque curves (rpm, Nm) added: a point at every rpm either has, each curve read linearly between
// its points and held at its end values outside them. Both sorted by rpm; so is the result.
std::vector<glm::vec2> AddTorqueCurves(const std::vector<glm::vec2>& a, const std::vector<glm::vec2>& b);

// What the engine's torque at these revs is of the curve's (which has the turbos' full boost) when
// the turbos together give `boost`: (1 + boost) / (1 + full boost) on the engine's part, a hybrid's
// motor torque left as it is. 1 without turbos.
float VehicleTurboTorqueScale(const VehicleSettings& settings, float rpm, float boost);

// What a car's controllers read, in Assetto Corsa's units: the steering wheel (degrees, right positive),
// speed (km/h), pedals (0 to 1), lateral acceleration (g, left positive), the gear, the axles' slip
// angles (degrees: average of the two wheels' magnitudes, and the larger magnitude) and the oversteer
// factor (rear average less front average, degrees: our reading of the game's, which is undocumented).
struct VehicleControllerInputs
{
    float steerDegrees = 0.0f;
    float speedKmh = 0.0f;
    float gas = 0.0f;
    float brake = 0.0f;
    float lateralG = 0.0f;
    float gear = 0.0f;
    float slipAngleFrontAverage = 0.0f;
    float slipAngleFrontMax = 0.0f;
    float slipAngleRearAverage = 0.0f;
    float slipAngleRearMax = 0.0f;
    float oversteerFactor = 0.0f;
};

// A controller input by its name (STEER_DEG, SPEED_KMH, GAS, BRAKE, LATG, GEAR, SLIPANGLE_FRONT_AVERAGE,
// ..., OVERSTEER_FACTOR); 0 for one it does not know.
float ReadVehicleControllerInput(const VehicleControllerInputs& inputs, const std::string& name);

// Assetto Corsa's controller chain (as gro-ove's ac-torque-helper, acController.jsx, has it): from 0, each
// controller reads its input through its curve, adds it (ADD) or multiplies by it (MULT), and the value is
// kept within that controller's limits. Each controller's curve value is low-passed by its FILTER, a
// per-step factor at the game's 333 Hz, here a = FILTER^(dt * 333) (our reading: the reference applies
// none). `filtered` holds each controller's last value (resized to fit; empty starts them unfiltered);
// dt <= 0 skips the filter.
float EvaluateVehicleControllers(const std::vector<VehicleController>& controllers, const VehicleControllerInputs& inputs,
                                 std::vector<float>& filtered, float dt);

// The torque (Nm, at the transfer case) a centre coupling passes from the rear to the front:
// rampTorque times the shafts' speed difference, finalDrive * (rearWheelSpeed - frontWheelSpeed) (wheel
// speeds in rad/s, axle averages), held within maxTorque either way.
float ComputeCentreCouplingTorque(float rampTorque, float maxTorque, float finalDrive, float rearWheelSpeed, float frontWheelSpeed);

// The rear wheels' steer angle (radians, right positive as the steering is) from the rear steering
// controllers' output (radians): an output with the steering's sign turns the rear against the front
// (our reading of the game's: the Porsche 991's turns them against the front at low speed and with
// it above 80 km/h, as the real car does).
inline float ComputeRearSteerAngle(float controllerOutput)
{
    return -controllerOutput;
}

// The body's collision parts in vehicle space: the car's boxes placed from the centre of mass
// (chassisCenter + centerOfMassOffset), and the shell's points with any below the ground-touching boxes'
// highest top raised to it. No boxes: no parts (the body is the chassis box).
struct VehicleChassisBox
{
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{0.0f};
};
struct VehicleChassisParts
{
    std::vector<VehicleChassisBox> boxes;
    std::vector<glm::vec3> hull;
};
VehicleChassisParts BuildChassisParts(const VehicleSettings& settings);

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
    // A sequential manual gearbox with an automatic clutch instead of the automatic one: the driver
    // changes gear (gearShifts), the throttle only drives and pulling it back only brakes.
    bool manualGearbox = false;
    // Gear changes asked for since the last controls: +1 per change up, -1 per change down.
    int gearShifts = 0;
    // Asked for neutral since the last controls (after any gearShifts).
    bool selectNeutral = false;
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

// The same for the manual gearbox in `gear` (-1 reverse, 0 neutral, 1 and up forward): the throttle
// drives the way the gear does (negative in reverse, as the automatic's), pulling it back brakes, and
// the hand brake leaves the throttle alone (the gearbox opens the clutch instead).
VehicleDriverInput ResolveManualDriverInput(const VehicleControls& controls, int gear);

// Where an automatic gearbox changes gear, in engine rpm: up at upLight on a light throttle and at
// upFull on a full one, down at downClosed off the throttle and at downFull (the kickdown) on a
// full one; the throttle blends between them.
struct VehicleShiftPoints
{
    float upLight = 0.0f;
    float upFull = 0.0f;
    float downClosed = 0.0f;
    float downFull = 0.0f;
};

// The shift points from the settings' shiftUpRpm and shiftDownRpm, or from the engine's rev range
// where those are 0. Every point is above the idle, so the gearbox changes down before the engine
// would have to turn slower than it can.
VehicleShiftPoints ComputeVehicleShiftPoints(const VehicleSettings& settings);

// An automatic gearbox: its ratios, shift points and timing.
struct VehicleGearbox
{
    std::vector<float> forwardRatios; // first gear up
    float reverseRatio = 0.0f;        // its size
    VehicleShiftPoints shiftPoints;
    float idleRpm = 1000.0f;
    float switchSeconds = 0.5f;  // the clutch is open while the gears change
    float switchDownSeconds = 0.0f; // a change down's (0: switchSeconds)
    float upshiftCutSeconds = 0.0f; // a change up cuts the engine this long (0: only while the clutch is open)
    float releaseSeconds = 0.3f; // then it bites over this long
    float latencySeconds = 0.5f; // and the box waits this long before another change
    float launchRpm = 0.0f;      // moving off, the clutch slips around this engine rpm (0: none)
    float limiterRpm = 0.0f;     // the manual box refuses a change down that would rev past this (0: no guard)
};

// The gearbox's state, kept by the caller between steps. `gear` is 1 and up forward, -1 reverse;
// `clutch` is the clutch's friction from 0 (open) to 1, which the caller hands to the physics with
// the gear and by which it scales the throttle (the engine is cut while the clutch is open).
// `revMatch` is set while the gears change: the engine then follows the new gear's speed.
struct VehicleGearboxState
{
    int gear = 1;
    float clutch = 1.0f;
    float switchLeft = 0.0f;
    float releaseLeft = 0.0f;
    float latencyLeft = 0.0f;
    // What is left of an upshift's engine cut: the caller shuts the throttle meanwhile.
    float cutLeft = 0.0f;
    bool idling = false;
    bool revMatch = false;
    // Moving off with the clutch slipping on the engine's revs (gearbox.launchRpm): the caller then keeps
    // the throttle whole rather than scaling it by the clutch.
    bool launching = false;
};

// The engine rpm the gearbox's output turns it at in `gear`; 0 in a gear the box does not have.
// `outputRpm` is the gearbox's output speed before the gear: the wheels' speed times the final drive.
float VehicleGearRpm(const VehicleGearbox& gearbox, int gear, float outputRpm);

// One step of the automatic gearbox. It decides by the speed the car travels at (`outputRpm`, from the
// car's speed over the ground rather than the engine's own revs, which flare while the clutch slips
// and cannot fall below the idle), changing up or down as many gears as the throttle's shift points
// ask for, and keeps the gears out of hunting: it only changes up when the next gear stays above the
// point it would change down at, and the other way round. `forward` is the driver's signed throttle:
// its sign selects drive or reverse. Off the throttle and below the idle in gear, the clutch opens so
// the car rolls to a stop rather than the engine pushing it.
//
// `engineRpm` is the engine's own speed. Moving off on the throttle (from rest, or rolling below the
// idle) with a launch rpm, the clutch slips on it: open below 95 % of the launch rpm, shut at 115 %, so
// the engine settles where its torque is what the clutch passes and the car pulls on the engine's
// torque there rather than at the idle; the launch ends once the wheels turn the engine at its speed
// (part throttle launches proportionally nearer the idle). The engine's speed also brings an upshift on
// when it passes the point while the wheels lag it (the clutch slipping for traction control).
void UpdateAutomaticGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, float forward, float outputRpm, float deltaSeconds,
                            float engineRpm = 0.0f);

// One step of a sequential manual gearbox with an automatic clutch: `shifts` changes up (positive) or
// down (negative) one gear each, through neutral (0) between first and reverse. It refuses a change down
// that would rev the engine past gearbox.limiterRpm, and reverse while the car still rolls faster than
// the idle in it. A change under way opens the clutch, matches the revs and lets the clutch bite as the
// automatic's does; moving off, rolling below the idle off the throttle and the launch work as there.
// In neutral, and while `declutch` holds (the hand brake), the clutch is open; let go, it bites again.
// `neutral` takes it out of gear after the shifts.
void UpdateManualGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, int shifts, bool neutral, float forward, float outputRpm,
                         float deltaSeconds, float engineRpm = 0.0f, bool declutch = false);
}
