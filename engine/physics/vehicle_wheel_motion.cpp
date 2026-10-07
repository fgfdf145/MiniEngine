#include "vehicle_wheel_motion.h"

#include <cmath>

namespace me
{

VehicleWheelMotion ComputeVehicleWheelMotion(
    const PhysicsPose& body,
    const PhysicsPose& wheel,
    const glm::quat& vehicleToModel,
    const glm::vec3& scale)
{
    const glm::quat bodyInverse = glm::conjugate(body.rotation);
    const glm::quat modelToVehicle = glm::conjugate(vehicleToModel);
    // The offset in double, where the two world positions still differ in every digit; small, it fits a float.
    const glm::vec3 offsetInVehicle = bodyInverse * glm::vec3(wheel.position - body.position);
    const glm::quat rotationInVehicle = bodyInverse * wheel.rotation;

    // The steering turns the axle about Y and the roll leaves it alone, so the axle's heading is the
    // steering and what is left is the roll.
    const glm::vec3 axle = rotationInVehicle * glm::vec3(1.0f, 0.0f, 0.0f);
    const float steerAngle = std::atan2(-axle.z, axle.x);
    const glm::quat steerInVehicle = glm::angleAxis(steerAngle, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat spinInVehicle = glm::conjugate(steerInVehicle) * rotationInVehicle;

    // A rotation seen from the model's axes is conjugated by the change of axes.
    VehicleWheelMotion motion;
    motion.center = (modelToVehicle * offsetInVehicle) / scale;
    motion.steer = modelToVehicle * steerInVehicle * vehicleToModel;
    motion.spin = modelToVehicle * spinInVehicle * vehicleToModel;
    return motion;
}
}
