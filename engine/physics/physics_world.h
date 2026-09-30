#pragma once

#include "vehicle_settings.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace me
{

using VehicleId = uint32_t;

struct PhysicsPose
{
    glm::vec3 position{0.0f};
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
    glm::vec3 mount{0.0f};
    glm::vec3 suspensionAxis{0.0f, -1.0f, 0.0f};
    // The travel: the suspension's length at full bump and at full droop, and what the spring holds at
    // rest under the car's weight is up to the caller (ComputeRestSuspensionLength).
    float suspensionMinLength = 0.0f;
    float suspensionMaxLength = 0.0f;
    float radius = 0.0f;
    float width = 0.0f;
    // The rest apply while inContact: where the tyre meets the ground, the ground's normal, and the
    // tyre's rolling and sideways directions along it.
    glm::vec3 contactPosition{0.0f};
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
};

struct VehicleTelemetry
{
    float forwardSpeed = 0.0f; // metres per second along the car's +Z, negative when reversing
    float engineRpm = 0.0f;
    int gear = 0; // negative reverse, 0 neutral, then the forward gears from 1
    uint32_t wheelsInContact = 0;
};

// A Jolt Physics world holding static collision geometry and wheeled vehicles, all in world space
// with +Y up. Jolt itself stays inside physics_world.cpp.
//
// Update advances the simulation in fixed steps and carries the remainder over to the next call;
// poses read back are interpolated between the last two steps, so motion stays smooth at any frame
// rate.
class PhysicsWorld
{
  public:
    static constexpr float kFixedStepSeconds = 1.0f / 1000.0f;
    // A long frame (a hitch, a breakpoint) runs at most this many steps and drops the rest, rather
    // than taking ever longer to catch up.
    static constexpr int kMaxStepsPerUpdate = 50;
    // The friction coefficient of static geometry that names none: a road. A wheel's grip is the
    // square root of its tyre's coefficient times the surface's, so Jolt's own default of 0.2 would
    // leave a car with half the grip of a tyre on tarmac.
    static constexpr float kDefaultSurfaceFriction = 1.0f;

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // A triangle mesh that never moves, in world space, three indices per triangle, of a surface with
    // this friction coefficient (about 1 for tarmac, 0.6 for grass). Degenerate triangles are dropped.
    // False, adding nothing, when no triangle is left.
    bool AddStaticMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices, float friction = kDefaultSurfaceFriction);
    void AddStaticBox(
        const glm::vec3& center,
        const glm::vec3& halfExtents,
        const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        float friction = kDefaultSurfaceFriction);
    size_t GetStaticBodyCount() const;
    size_t GetStaticTriangleCount() const;

    // A car at this pose (its vehicle space's origin), at rest.
    VehicleId AddVehicle(const VehicleSettings& settings, const PhysicsPose& pose);
    void SetVehicleControls(VehicleId vehicle, const VehicleControls& controls);
    // Puts the car at the pose, stopped, as it was when added.
    void ResetVehicle(VehicleId vehicle, const PhysicsPose& pose);
    PhysicsPose GetVehiclePose(VehicleId vehicle) const;
    std::vector<VehicleWheelState> GetVehicleWheels(VehicleId vehicle) const;
    VehicleTelemetry GetVehicleTelemetry(VehicleId vehicle) const;

    // Returns how many fixed steps ran.
    int Update(float deltaSeconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
