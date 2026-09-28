#include "vehicle_settings.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me
{

namespace
{
// Below this the car counts as stopped, and throttle the other way selects the other direction.
constexpr float kDirectionChangeSpeed = 0.5f; // m/s
constexpr float kGravity = 9.81f;
}

VehicleSettings FitVehicleSettingsToBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds, const VehicleSettings& tuning)
{
    // A model with no extent along an axis (a placeholder, a flat card) still gets a drivable car.
    constexpr glm::vec3 kMinimumSize(0.5f, 0.3f, 0.8f);
    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    const glm::vec3 size = glm::max(maxBounds - minBounds, kMinimumSize);
    const float floorY = center.y - size.y * 0.5f;

    VehicleSettings settings = tuning;
    // A road car 1.4 m tall rides on wheels of about 0.32 m.
    settings.wheelRadius = std::clamp(size.y * 0.23f, 0.12f, 0.6f);
    settings.wheelWidth = std::clamp(size.x * 0.12f, 0.08f, 0.4f);
    settings.trackCenterX = center.x;
    settings.halfTrackWidth = std::max(size.x * 0.5f - settings.wheelWidth * 0.6f, settings.wheelWidth);
    // Overhangs of about a fifth of the length at either end.
    settings.frontAxleZ = center.z + size.z * 0.5f - std::max(size.z * 0.18f, settings.wheelRadius * 1.1f);
    settings.rearAxleZ = center.z - size.z * 0.5f + std::max(size.z * 0.2f, settings.wheelRadius * 1.1f);
    settings.suspensionMaxLength = std::clamp(settings.wheelRadius, 0.15f, 0.35f);
    settings.suspensionMinLength = settings.suspensionMaxLength * 0.15f;

    // The contact patch sits on the floor once the springs carry the car's weight.
    settings.wheelMountY = floorY + settings.wheelRadius + ComputeRestSuspensionLength(settings, kGravity);

    // The chassis box clears the ground by most of a wheel radius and reaches the roof.
    const float chassisBottom = floorY + settings.wheelRadius * 0.8f;
    const float chassisTop = std::max(center.y + size.y * 0.5f, chassisBottom + 0.1f);
    settings.chassisCenter = glm::vec3(center.x, (chassisBottom + chassisTop) * 0.5f, center.z);
    settings.chassisHalfExtents = glm::vec3(size.x * 0.5f, (chassisTop - chassisBottom) * 0.5f, size.z * 0.5f);

    // A road car's centre of mass is around a third of its height above the ground.
    const float centerOfMassY = floorY + std::max(size.y * 0.33f, settings.wheelRadius * 1.2f);
    settings.centerOfMassOffset = glm::vec3(0.0f, centerOfMassY - settings.chassisCenter.y, 0.0f);
    return settings;
}

float ComputeRestSuspensionLength(const VehicleSettings& settings, float gravity)
{
    // The spring is a harmonic oscillator of the wheel's share of the mass, so its static deflection
    // is g / omega^2 whatever that share.
    const float omega = 2.0f * std::numbers::pi_v<float> * std::max(settings.suspensionFrequencyHz, 0.01f);
    const float deflection = gravity / (omega * omega);
    return std::clamp(
        settings.suspensionMaxLength - deflection,
        settings.suspensionMinLength,
        settings.suspensionMaxLength);
}

VehicleDriverInput ResolveVehicleDriverInput(const VehicleControls& controls, float forwardSpeed, float& direction)
{
    if (direction == 0.0f)
    {
        direction = 1.0f;
    }

    VehicleDriverInput input;
    input.right = std::clamp(controls.steering, -1.0f, 1.0f);
    input.brake = std::clamp(controls.brake, 0.0f, 1.0f);
    input.handBrake = std::clamp(controls.handBrake, 0.0f, 1.0f);

    const float throttle = std::clamp(controls.throttle, -1.0f, 1.0f);
    if (throttle * direction < 0.0f)
    {
        // Asking for the other direction: brake while the car still rolls the current way.
        if (forwardSpeed * direction > kDirectionChangeSpeed)
        {
            input.brake = std::max(input.brake, std::abs(throttle));
        }
        else
        {
            direction = throttle > 0.0f ? 1.0f : -1.0f;
            input.forward = throttle;
        }
    }
    else
    {
        input.forward = throttle;
    }

    if (input.handBrake > 0.0f)
    {
        input.forward = 0.0f;
    }
    return input;
}
}
