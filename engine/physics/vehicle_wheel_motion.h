#pragma once

#include "physics_world.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace me
{

// Where a wheel is on its car and how it is turned, in the car model's own space (before the
// entity's scale), for drawing the model's wheels where the simulation has them.
struct VehicleWheelMotion
{
    // The wheel's centre.
    glm::vec3 center{0.0f};
    // The wheel's turn about the vertical from the steering, and its roll about the axle.
    glm::quat steer{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat spin{1.0f, 0.0f, 0.0f, 0.0f};
};

// `wheel` and `body` are world poses as PhysicsWorld reports them (the wheel's rotation is the
// identity for a wheel that is straight and not rolled); `vehicleToModel` turns vehicle space into
// the model's (a half turn about Y for a car that faces -Z) and `scale` is the entity's.
VehicleWheelMotion ComputeVehicleWheelMotion(
    const PhysicsPose& body,
    const PhysicsPose& wheel,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale);
}
