#pragma once

#include "vehicle_settings.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <array>
#include <utility>
#include <vector>

namespace me
{

using VehicleId = uint32_t;

// World positions are in double: a world reaches thousands of metres from its origin (the GTA map puts
// Vice City at x + 6000 m), where floats are some 0.5 mm apart and a slow car's step would round away.
// Directions, forces and anything relative to the car stay in float.
struct PhysicsPose
{
    glm::dvec3 position{0.0};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct VehicleWheelState
{
    // World space. The wheel's own X axis is its axle, pointing to the car's left. The rotation includes
    // the roll about it, which is spinAngle.
    PhysicsPose pose;
    // How far the wheel has rolled about its axle (radians, folded to one turn, positive rolling the car
    // forward), and how far it rolled in the last fixed step, which may be several turns' worth of
    // half-turns: a wheel spinning faster than half a turn per step (over 3141 rad/s at 1000 Hz) cannot be
    // read from two poses, which look like it turned the other way.
    float spinAngle = 0.0f;
    float spinStep = 0.0f;
    bool inContact = false;
    float suspensionLength = 0.0f;
    float angularVelocity = 0.0f; // radians per second about the axle

    // What the last fixed step did at this wheel, for drawing it. World space, forces in newtons.
    // Where the suspension hangs from, and the way it extends (unit, from the mount to the wheel).
    glm::dvec3 mount{0.0};
    glm::vec3 suspensionAxis{0.0f, -1.0f, 0.0f};
    // The travel: the suspension's length at full bump and at full droop, and what the spring holds at
    // rest under the car's weight is up to the caller (ComputeRestSuspensionLength).
    float suspensionMinLength = 0.0f;
    float suspensionMaxLength = 0.0f;
    float radius = 0.0f;
    float width = 0.0f;
    // The rest apply while inContact: where the tyre meets the ground, the ground's normal, and the
    // tyre's rolling and sideways directions along it.
    glm::dvec3 contactPosition{0.0};
    glm::vec3 contactNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 contactLongitudinal{0.0f, 0.0f, 1.0f};
    glm::vec3 contactLateral{1.0f, 0.0f, 0.0f};
    // The spring and damper's push along the normal (the tyre's load), and the tyre's grip along
    // contactLongitudinal (drive and brake) and contactLateral (cornering).
    float suspensionForce = 0.0f;
    // What the brake can hold at this wheel at full pedal, in newton metres, as the step had it.
    float brakeTorque = 0.0f;
    float longitudinalForce = 0.0f;
    float lateralForce = 0.0f;
    // The slip that the grip follows: the wheel's speed against the ground's along the wheel (a ratio),
    // and the angle between the tyre and the way it is moving, in degrees.
    float slipRatio = 0.0f;
    float slipAngleDegrees = 0.0f;
    // The friction coefficients the tyre's curves gave at that slip, combined with the ground's: the force
    // the tyre makes is this times suspensionForce. And the most the curves give at any slip, likewise
    // combined: the tyre's peak grip, which the slip runs up to and then falls back from.
    float longitudinalFriction = 0.0f;
    float lateralFriction = 0.0f;
    float longitudinalPeakFriction = 0.0f;
    float lateralPeakFriction = 0.0f;

    // With a multibody suspension (VehicleSettings::frontSuspension/rearSuspension): the wheel's
    // travel from the design position (bump positive, m), its camber (top outward positive) and toe
    // (toe-in positive) in degrees relative to the body, the spring/damper/stop force at the wheel and
    // the anti-roll bar's, N.
    bool multibody = false;
    float travel = 0.0f;
    float camberDegrees = 0.0f;
    float toeDegrees = 0.0f;
    float springForce = 0.0f;
    float antiRollBarForce = 0.0f;
    // When the data gives the axle a hub mass and a tyre rate the wheel is a mass of its own on the
    // tyre's spring: the pose and suspensionLength are then the hub's, which the tyre's deflection
    // (m) keeps above where the physics engine touches the ground.
    bool unsprungMass = false;
    float tyreDeflection = 0.0f;
    // With the brush tyre (VehicleTyreModel::Brush): the load it worked with (N; the suspension's push
    // as foreseen before the step, which suspensionForce gives as it came out), its aligning moment about
    // the normal (N m), the share of the load on sliding bristles, and the carcass's shift fore-aft and
    // sideways (m) and twist (rad) against the rim.
    bool brushTyre = false;
    float tyreLoad = 0.0f;
    float aligningTorque = 0.0f;
    float slidingShare = 0.0f;
    glm::vec3 carcassDeflection{0.0f};
    // The brush tyre's contact patch, for drawing it: the carcass's bending shape factor Psi (its centre
    // line across the patch is y_c + theta_c (x - x_c) - y_c Psi/2 (x - x_c)^2, x forward and y left
    // along contactLongitudinal and -contactLateral), whether the tread runs through it forwards, and
    // each rib's place across the tread, contact length and stuck length from the leading edge (m).
    struct BrushRib
    {
        float y = 0.0f;
        float length = 0.0f;
        float stuckLength = 0.0f;
    };
    static constexpr int kMaxBrushRibs = 128;
    float carcassBendingShape = 0.0f;
    bool treadRollingForward = true;
    int brushRibCount = 0;
    std::array<BrushRib, kMaxBrushRibs> brushRibs{};
};

// A car's suspension linkage where the last step left it, in world space, for drawing: the rods and
// arms, the wheel carriers (uprights, or a solid axle's beam), the chassis pivots and the joints on the
// moving parts. Empty for a car on straight springs.
struct VehicleLinkage
{
    std::vector<std::array<glm::dvec3, 2>> links;
    std::vector<std::array<glm::dvec3, 2>> carriers;
    std::vector<glm::dvec3> chassis;
    std::vector<glm::dvec3> joints;
};

struct VehicleTelemetry
{
    float forwardSpeed = 0.0f; // metres per second along the car's +Z, negative when reversing
    float rightSpeed = 0.0f;   // metres per second to the car's right (its -X)
    // The front axle's centre to the car's right (m/s): the body's sideways speed plus what its yaw adds
    // that far ahead, so atan2(frontAxleRightSpeed, forwardSpeed) is the way the front wheels travel.
    float frontAxleRightSpeed = 0.0f;
    float engineRpm = 0.0f;
    int gear = 0; // negative reverse, 0 neutral, then the forward gears from 1
    // The clutch's friction from 0 (open, while the gears change) to 1 (shut, the drive connected).
    float clutch = 1.0f;
    // The automatic holds a gear the driver changed to (VehicleControls::gearShifts without manualGearbox).
    bool manualHold = false;
    uint32_t wheelsInContact = 0;
    // A coupled four-wheel drive's centre coupling in the last step (Nm at the transfer case, rear to front
    // positive), and the rear wheels' steer angle (degrees, right positive) for a car with rear steering.
    float centreCouplingTorque = 0.0f;
    float rearSteerDegrees = 0.0f;
    // The power the drivetrain lost in the last step (VehicleSettings::drivetrainLosses; kW): in the gears'
    // meshes, and to churning and the bearings.
    float drivetrainMeshLossKw = 0.0f;
    float drivetrainSpinLossKw = 0.0f;
    // The turbos' boost together now (the torque is the engine's times one plus it); 0 without turbos.
    float turboBoost = 0.0f;
    // The most any wheel on the ground turns slower than the road under it (the slip ratio's braking
    // side as a magnitude: 1 is locked) and faster than it (wheelspin), in the last step. Both are 0
    // below about 2 m/s, where a ratio against a crawling road says nothing.
    float lockSlip = 0.0f;
    float spinSlip = 0.0f;
    // The car's anti-lock brakes have a wheel's brake off, and its traction control has the throttle cut,
    // in the last step.
    bool absActive = false;
    bool tractionControlCut = false;
    // Water (PhysicsWorld::AddWaterSurface): the share of the body's shape below the surface, how far
    // the car has filled (0 dry, 1 full: it floats while it fills and then sinks), and whether the water
    // has reached the engine, which then gives no more drive.
    float submergedShare = 0.0f;
    float flooded = 0.0f;
    bool engineDrowned = false;
    // Each wheel's tyre with VehicleSettings::tyreTemperatures on (simulated): its tread's lanes round the tyre
    // (inside, middle, outside: the car's centre side first), its core (C), its pressure (psi) and the grip
    // temperature and pressure leave it (1 at their best).
    struct TyreTemperatures
    {
        bool simulated = false;
        std::array<float, 3> tread{};
        float core = 0.0f;
        float pressure = 0.0f;
        float grip = 1.0f;
    };
    std::array<TyreTemperatures, 4> tyres{};
    // And its wear with VehicleSettings::tyreWear on: the virtual km slid, graining and blistering (0 to 100) and
    // the grip they leave it.
    struct TyreWear
    {
        bool simulated = false;
        float virtualKm = 0.0f;
        float grain = 0.0f;
        float blister = 0.0f;
        float grip = 1.0f;
    };
    std::array<TyreWear, 4> wear{};
};

// A Jolt Physics world holding static collision geometry and wheeled vehicles, all in world space
// with +Y up. Jolt itself stays inside physics_world.cpp.
//
// Update advances the simulation in fixed steps and carries the remainder over to the next call;
// poses read back are interpolated between the last two steps, so motion stays smooth at any frame
// rate.
// How a ground surface grips a tyre. friction is a ratio to dry asphalt that multiplies the tyre's own
// coefficient (as Assetto Corsa's surfaces.ini FRICTION does), which suits paved surfaces, where the
// rubber decides. On loose, soft and frozen ground the surface decides, and a stickier tyre gains
// little: frictionCap is then the absolute coefficient the contact cannot pass. Wet surfaces lose
// grip with speed, friction times exp(-wetSpeedFalloff * speed in m/s). On loose ground the grip
// hardly falls past its peak: slidingShare is the sliding friction's share of the peak there (0
// keeps the tyre's own). rollingResistance is added to the tyre's own coefficient on soft ground.
struct SurfaceGrip
{
    float friction = 1.0f;
    float frictionCap = 0.0f;
    float wetSpeedFalloff = 0.0f;
    float slidingShare = 0.0f;
    float rollingResistance = 0.0f;
};

class PhysicsWorld
{
  public:
    // The fixed step a new world runs at (1000 Hz), and the range SetStepSeconds takes (4000 Hz to 60 Hz).
    static constexpr float kDefaultStepSeconds = 1.0f / 1000.0f;
    static constexpr float kMinStepSeconds = 1.0f / 4000.0f;
    static constexpr float kMaxStepSeconds = 1.0f / 60.0f;
    // A long frame (a hitch, a breakpoint) catches up at most this much simulated time and drops the
    // rest, rather than taking ever longer to catch up (MaxStepsPerUpdate steps).
    static constexpr float kMaxCatchUpSeconds = 0.05f;
    // The friction of static geometry that names none: a road. A wheel's grip is its tyre's coefficient
    // times the surface's ratio (see SurfaceGrip), so Jolt's own default of 0.2 would leave a car with a
    // fifth of its grip.
    static constexpr float kDefaultSurfaceFriction = 1.0f;

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // A triangle mesh that never moves, three indices per triangle, of a surface with this friction
    // coefficient (about 1 for tarmac, 0.6 for grass). Its vertices are relative to `origin` in world
    // space: kept near their own origin they keep their float precision however far out the mesh lies.
    // Degenerate triangles are dropped. False, adding nothing, when no triangle is left.
    bool AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, float friction = kDefaultSurfaceFriction);
    bool AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, const SurfaceGrip& grip, const glm::dvec3& origin = glm::dvec3(0.0));
    void AddStaticBox(
        const glm::dvec3& center,
        const glm::vec3& halfExtents,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        float friction = kDefaultSurfaceFriction);
    // The top of the world's water, world-space triangles seen from above (WaterSurface). Below it a
    // car is held up by buoyancy and slowed by drag while it fills, then sinks, and once the water is
    // over most of it the engine drowns and gives no more drive, as in the games the maps come from.
    // In float: the surface only sets the buoyancy, which half a millimetre does not change.
    void AddWaterSurface(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices);
    size_t GetWaterTriangleCount() const;
    size_t GetStaticBodyCount() const;
    size_t GetStaticTriangleCount() const;

    // A car at this pose (its vehicle space's origin), at rest.
    VehicleId AddVehicle(const VehicleSettings& settings, const PhysicsPose& pose);
    void SetVehicleControls(VehicleId vehicle, const VehicleControls& controls);
    // Recuts a car's brush tyres into this many ribs and segments along each (0 for the tyre's own count)
    // while it drives; their carcasses start again from rest. Nothing for a car on the physics engine's tyres.
    void SetVehicleBrushTyreBristles(VehicleId vehicle, int ribs, int segmentsPerRib);
    // Switches a car's anti-lock brakes and traction control on or off while it drives (useAbs and
    // useTractionControl): a car without one stays without it, one switched on works as the car's
    // settings give it.
    void SetVehicleDriverAids(VehicleId vehicle, bool abs, bool tractionControl);
    // Puts the car at the pose, stopped, as it was when added.
    void ResetVehicle(VehicleId vehicle, const PhysicsPose& pose);
    // The height of the first static surface (track, ground, walls) straight below `from`, within
    // `maxDistance`; cars are not hit. Empty when there is none.
    std::optional<double> FindGroundBelow(const glm::dvec3& from, double maxDistance) const;
    PhysicsPose GetVehiclePose(VehicleId vehicle) const;
    std::vector<VehicleWheelState> GetVehicleWheels(VehicleId vehicle) const;
    VehicleLinkage GetVehicleLinkage(VehicleId vehicle) const;
    VehicleTelemetry GetVehicleTelemetry(VehicleId vehicle) const;
    // The body's collision shape's bounds in vehicle space (the chassis box, or the car's boxes and shell).
    std::pair<glm::vec3, glm::vec3> GetVehicleBodyBounds(VehicleId vehicle) const;

    // The length of one fixed step. A new step (clamped to kMinStepSeconds..kMaxStepSeconds) takes
    // effect from the next Update, also while cars drive: a shorter one follows stiff springs, kerbs and
    // the tyres' contact more closely at a cost in time that grows in step with the rate; a longer one
    // is cheaper but can make a stiff car or the brush tyre ring or go unstable.
    void SetStepSeconds(float seconds);
    float GetStepSeconds() const;
    // How many steps a long frame runs at most, kMaxCatchUpSeconds at the current step.
    int MaxStepsPerUpdate() const;

    // Returns how many fixed steps ran. With a wall-clock budget (seconds, 0 for none) the steps stop
    // once that much real time has gone and the backlog is dropped as for a capped frame: a world too
    // slow to keep up with real time (a debug build, a heavy car) then runs in slow motion instead of
    // taking the frame rate down with it.
    int Update(float deltaSeconds, float wallBudgetSeconds = 0.0f);
    // Runs exactly `count` fixed steps (at most MaxStepsPerUpdate), whatever time was carried over, and
    // then carries `carrySeconds` (under a step) over instead, which places the poses read back between
    // the last two steps as Update would have: a replayed drive takes the steps its recording took, frame
    // by frame, and reads back as it did. Returns how many ran.
    int RunSteps(int count, float carrySeconds = 0.0f);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
