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

VehicleSettings FitVehicleSettingsToBounds(
    const glm::vec3& minBounds,
    const glm::vec3& maxBounds,
    const VehicleSettings& tuning,
    const VehicleWheelLayout* wheelLayout)
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
    settings.hasWheelLayout = wheelLayout != nullptr;
    glm::vec3 centerOfWheels(0.0f);
    if (wheelLayout != nullptr)
    {
        // Left wheels are the even ones: +X is the car's left.
        settings.wheelLayout = *wheelLayout;
        float radius = 0.0f;
        float width = 0.0f;
        glm::vec3 sum(0.0f);
        for (const VehicleWheelGeometry& wheel : *wheelLayout)
        {
            radius += wheel.radius * 0.25f;
            width += wheel.width * 0.25f;
            sum += wheel.center * 0.25f;
        }
        settings.wheelRadius = std::clamp(radius, 0.05f, 1.0f);
        settings.wheelWidth = std::clamp(width, 0.05f, 0.6f);
        centerOfWheels = sum;
        settings.trackCenterX = sum.x;
        settings.halfTrackWidth = ((*wheelLayout)[0].center.x - (*wheelLayout)[1].center.x) * 0.5f;
        settings.frontAxleZ = ((*wheelLayout)[0].center.z + (*wheelLayout)[1].center.z) * 0.5f;
        settings.rearAxleZ = ((*wheelLayout)[2].center.z + (*wheelLayout)[3].center.z) * 0.5f;
    }
    settings.suspensionMaxLength = std::clamp(settings.wheelRadius, 0.15f, 0.35f);
    settings.suspensionMinLength = settings.suspensionMaxLength * 0.15f;

    // The contact patch sits on the floor once the springs carry the car's weight.
    const float restLength = ComputeRestSuspensionLength(settings, kGravity);
    settings.wheelMountY = (wheelLayout != nullptr ? centerOfWheels.y : floorY + settings.wheelRadius) + restLength;

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

VehicleWheelGeometry GetVehicleWheelMount(const VehicleSettings& settings, size_t index)
{
    const bool left = index % 2 == 0;
    const bool front = index < 2;
    VehicleWheelGeometry mount;
    mount.radius = settings.wheelRadius;
    mount.width = settings.wheelWidth;
    if (settings.hasWheelLayout && index < kVehicleWheelCount)
    {
        const VehicleWheelGeometry& wheel = settings.wheelLayout[index];
        mount = wheel;
        // The centre rests the spring's rest length below the mount.
        mount.center.y += ComputeRestSuspensionLength(settings, kGravity);
        return mount;
    }
    mount.center = glm::vec3(
        settings.trackCenterX + (left ? settings.halfTrackWidth : -settings.halfTrackWidth),
        settings.wheelMountY,
        front ? settings.frontAxleZ : settings.rearAxleZ);
    return mount;
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
