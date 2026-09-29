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
    // World space. The wheel's own X axis is its axle, pointing to the car's left.
    PhysicsPose pose;
    bool inContact = false;
    float suspensionLength = 0.0f;
    float angularVelocity = 0.0f; // radians per second about the axle
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
    static constexpr float kFixedStepSeconds = 1.0f / 60.0f;
    // A long frame (a hitch, a breakpoint) runs at most this many steps and drops the rest, rather
    // than taking ever longer to catch up.
    static constexpr int kMaxStepsPerUpdate = 5;
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
