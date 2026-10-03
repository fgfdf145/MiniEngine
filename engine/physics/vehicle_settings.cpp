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

// A curve's value at x by linear interpolation, its end values beyond the ends; 0 for no points.
float EvaluateCurve(const std::vector<glm::vec2>& curve, float x)
{
    if (curve.empty())
    {
        return 0.0f;
    }
    if (x <= curve.front().x)
    {
        return curve.front().y;
    }
    for (size_t index = 1; index < curve.size(); ++index)
    {
        if (x <= curve[index].x)
        {
            const float span = curve[index].x - curve[index - 1].x;
            const float t = span > 0.0f ? (x - curve[index - 1].x) / span : 1.0f;
            return curve[index - 1].y + (curve[index].y - curve[index - 1].y) * t;
        }
    }
    return curve.back().y;
}
}

float ComputeBrakeTorquePerWheel(const VehicleSettings& settings)
{
    if (settings.maxBrakeTorque > 0.0f)
    {
        return settings.maxBrakeTorque;
    }
    // Without tyre data the physics engine's own tyres peak at 1.2 and are combined with the surface's
    // friction (1 by default) by a square root.
    constexpr float kDefaultGrip = 1.1f;
    // Sharing the brakes by load gives the fronts more of the torque than the fixed split does, and they
    // lock first: the total is held a little lower so that the car keeps its steering.
    const float gripUsed = settings.dynamicBrakeBias ? 0.66f : 0.75f;
    const float front = settings.frontTyres.longitudinalGrip;
    const float rear = settings.rearTyres.longitudinalGrip;
    const float grip = front > 0.0f && rear > 0.0f ? 0.5f * (front + rear) : std::max(front, rear) > 0.0f ? std::max(front, rear) : kDefaultGrip;
    return gripUsed * grip * std::max(settings.massKg, 1.0f) * kGravity * std::max(settings.wheelRadius, 0.01f) * 0.25f;
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

    // A road car's centre of mass is around a third of its height above the ground, over the middle of
    // the model; the car's own data says where (its weight split between the axles, its height over
    // the ground under the tyres).
    const float groundY = wheelLayout != nullptr ? centerOfWheels.y - settings.wheelRadius : floorY;
    const float centerOfMassY = settings.centerOfMassHeight > 0.0f ? groundY + settings.centerOfMassHeight
                                                                  : floorY + std::max(size.y * 0.33f, settings.wheelRadius * 1.2f);
    glm::vec3 centerOfMass(settings.chassisCenter.x, centerOfMassY, settings.chassisCenter.z);
    if (settings.frontWeightShare > 0.0f)
    {
        centerOfMass.x = settings.trackCenterX;
        centerOfMass.z = settings.rearAxleZ + settings.frontWeightShare * (settings.frontAxleZ - settings.rearAxleZ);
    }
    settings.centerOfMassOffset = centerOfMass - settings.chassisCenter;
    return settings;
}

bool HasSuspensionGeometry(const VehicleSettings& settings)
{
    return settings.frontSuspension.type != VehicleSuspensionType::None && settings.rearSuspension.type != VehicleSuspensionType::None;
}

VehicleCarSpec WithStartingFuel(const VehicleCarSpec& spec)
{
    VehicleCarSpec fuelled = spec;
    fuelled.fuelLitres.reset();
    fuelled.fuelTankPosition.reset();
    if (!spec.fuelLitres.has_value() || *spec.fuelLitres <= 0.0f || !spec.massKg.has_value() || *spec.massKg <= 0.0f)
    {
        return fuelled;
    }
    const float fuel = *spec.fuelLitres * kFuelKgPerLitre;
    const float mass = *spec.massKg + fuel;
    const glm::vec3 tank = spec.fuelTankPosition.value_or(glm::vec3(0.0f));
    fuelled.massKg = mass;
    // The tank's share of its weight on the front axle: its distance ahead of the rear axle over the
    // wheelbase (the centre of mass being frontWeightShare of the wheelbase ahead of it).
    if (spec.frontWeightShare.has_value() && spec.wheelbase.has_value() && *spec.wheelbase > 0.0f)
    {
        const float tankShare = *spec.frontWeightShare + tank.z / *spec.wheelbase;
        fuelled.frontWeightShare = (*spec.frontWeightShare * *spec.massKg + tankShare * fuel) / mass;
    }
    // The centre of mass moves up or down by the fuel's moment over the new mass.
    const float rise = fuel * tank.y / mass;
    for (std::optional<VehicleSuspensionAxle>* axle : {&fuelled.frontSuspension, &fuelled.rearSuspension})
    {
        if (axle->has_value())
        {
            (*axle)->centerOfMassAboveWheel += rise;
        }
    }
    return fuelled;
}

VehicleSettings ApplyCarSpec(const VehicleSettings& tuning, const VehicleCarSpec& dryspec)
{
    const VehicleCarSpec spec = WithStartingFuel(dryspec);
    VehicleSettings settings = tuning;
    if (spec.massKg.has_value() && *spec.massKg > 0.0f)
    {
        settings.massKg = *spec.massKg;
    }
    if (spec.frontWeightShare.has_value() && *spec.frontWeightShare > 0.0f && *spec.frontWeightShare < 1.0f)
    {
        settings.frontWeightShare = *spec.frontWeightShare;
    }
    if (spec.inertiaBox.has_value() && spec.inertiaBox->x > 0.0f && spec.inertiaBox->y > 0.0f && spec.inertiaBox->z > 0.0f)
    {
        settings.inertiaBox = *spec.inertiaBox;
    }
    // The centre of mass's height: each axle's tyre radius plus its height over the wheel centre
    // (centerOfMassAboveWheel, Assetto Corsa's -BASEY), by the weight on it.
    if (spec.frontSuspension.has_value() && spec.rearSuspension.has_value() && spec.frontSuspension->tyreRadius > 0.0f &&
        spec.rearSuspension->tyreRadius > 0.0f)
    {
        const float share = settings.frontWeightShare > 0.0f ? settings.frontWeightShare : 0.5f;
        const float front = spec.frontSuspension->tyreRadius + spec.frontSuspension->centerOfMassAboveWheel;
        const float rear = spec.rearSuspension->tyreRadius + spec.rearSuspension->centerOfMassAboveWheel;
        const float height = share * front + (1.0f - share) * rear;
        if (height > 0.0f)
        {
            settings.centerOfMassHeight = height;
        }
    }
    if (spec.drive.has_value())
    {
        settings.drive = *spec.drive;
    }
    if (spec.minRpm.has_value() && *spec.minRpm > 0.0f)
    {
        settings.minRpm = *spec.minRpm;
    }
    if (spec.maxRpm.has_value() && *spec.maxRpm > settings.minRpm)
    {
        settings.maxRpm = *spec.maxRpm;
    }

    if (spec.torqueCurve.size() >= 2)
    {
        settings.torqueCurve = spec.torqueCurve;
        std::sort(
            settings.torqueCurve.begin(),
            settings.torqueCurve.end(),
            [](const glm::vec2& a, const glm::vec2& b)
            {
                return a.x < b.x;
            });
        float peak = 0.0f;
        for (const glm::vec2& point : settings.torqueCurve)
        {
            peak = std::max(peak, point.y);
        }
        if (peak > 0.0f)
        {
            settings.maxEngineTorque = peak;
        }
        else
        {
            settings.torqueCurve.clear();
        }
    }

    if (!spec.gearRatios.empty())
    {
        settings.gearRatios = spec.gearRatios;
        if (spec.reverseGearRatio.has_value())
        {
            settings.reverseGearRatio = -std::abs(*spec.reverseGearRatio);
        }
        if (spec.finalDriveRatio.has_value() && *spec.finalDriveRatio > 0.0f)
        {
            settings.finalDriveRatio = *spec.finalDriveRatio;
        }
        // The shift points follow the engine's rev range (ComputeVehicleShiftPoints).
        settings.shiftUpRpm = 0.0f;
        settings.shiftDownRpm = 0.0f;
    }

    if (spec.gearSwitchSeconds.has_value() && *spec.gearSwitchSeconds > 0.0f)
    {
        settings.gearSwitchSeconds = *spec.gearSwitchSeconds;
    }
    if (spec.clutchReleaseSeconds.has_value() && *spec.clutchReleaseSeconds > 0.0f)
    {
        settings.clutchReleaseSeconds = *spec.clutchReleaseSeconds;
    }
    if (spec.engineInertia.has_value() && *spec.engineInertia > 0.0f)
    {
        settings.engineInertia = *spec.engineInertia;
    }
    if (!spec.aeroWings.empty())
    {
        // The air's drag and downforce come from the car's wings at their base angle; the body's own
        // damping, a stand-in for the drag, goes.
        settings.aeroSurfaces.clear();
        for (const VehicleAeroWing& wing : spec.aeroWings)
        {
            const float area = std::max(wing.chord, 0.0f) * std::max(wing.span, 0.0f);
            VehicleAeroSurface surface;
            surface.position = wing.position;
            surface.dragArea = std::max(EvaluateCurve(wing.dragCurve, wing.angleDegrees), 0.0f) * wing.dragGain * area;
            // A lift coefficient below zero pushes the car down.
            surface.downforceArea = -EvaluateCurve(wing.liftCurve, wing.angleDegrees) * wing.liftGain * area;
            settings.aeroSurfaces.push_back(surface);
        }
        settings.linearDamping = 0.0f;
    }
    if (spec.frontTyres.has_value())
    {
        settings.frontTyres = *spec.frontTyres;
    }
    if (spec.rearTyres.has_value())
    {
        settings.rearTyres = *spec.rearTyres;
    }

    if (spec.maxSteerAngleDegrees.has_value() && *spec.maxSteerAngleDegrees > 0.0f)
    {
        settings.maxSteerAngleDegrees = *spec.maxSteerAngleDegrees;
    }
    if (spec.brakeTorquePerWheel.has_value() && *spec.brakeTorquePerWheel > 0.0f)
    {
        settings.maxBrakeTorque = *spec.brakeTorquePerWheel;
    }
    if (spec.frontBrakeShare.has_value())
    {
        settings.frontBrakeShare = std::clamp(*spec.frontBrakeShare, 0.05f, 0.95f);
    }
    if (spec.handBrakeTorquePerWheel.has_value() && *spec.handBrakeTorquePerWheel >= 0.0f)
    {
        settings.maxHandBrakeTorque = *spec.handBrakeTorquePerWheel;
    }
    if (spec.suspensionFrequencyHz.has_value() && *spec.suspensionFrequencyHz > 0.0f)
    {
        settings.suspensionFrequencyHz = *spec.suspensionFrequencyHz;
    }
    if (spec.suspensionDamping.has_value() && *spec.suspensionDamping >= 0.0f)
    {
        settings.suspensionDamping = *spec.suspensionDamping;
    }
    if (spec.antiRollBars.has_value())
    {
        settings.antiRollBars = *spec.antiRollBars;
    }
    if (spec.limitedSlipDifferentials.has_value())
    {
        settings.limitedSlipDifferentials = *spec.limitedSlipDifferentials;
        // The game's lock under power is the share of the drive torque the clutch pack takes.
        if (spec.differentialPower.has_value() && *spec.differentialPower > 0.0f)
        {
            settings.limitedSlipLock = std::clamp(*spec.differentialPower, 0.05f, 1.0f);
        }
    }
    // The linkage only when both axles have one: half a multibody car would not drive.
    if (spec.frontSuspension.has_value() && spec.rearSuspension.has_value() && spec.frontSuspension->type != VehicleSuspensionType::None && spec.rearSuspension->type != VehicleSuspensionType::None)
    {
        settings.frontSuspension = *spec.frontSuspension;
        settings.rearSuspension = *spec.rearSuspension;
    }
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

VehicleShiftPoints ComputeVehicleShiftPoints(const VehicleSettings& settings)
{
    // Along the rev range from the idle to the limiter: full throttle changes up just under the
    // limiter and kicks down while the lower gear still has room to pull; a light foot changes up
    // early and the box holds a gear off the throttle until the engine nears the idle.
    const float idle = std::max(settings.minRpm, 1.0f);
    const float range = std::max(settings.maxRpm - idle, 1.0f);
    const float limiter = idle + range;
    VehicleShiftPoints points;
    points.upFull = settings.shiftUpRpm > 0.0f ? std::min(settings.shiftUpRpm, limiter) : idle + 0.93f * range;
    points.downClosed = settings.shiftDownRpm > 0.0f ? settings.shiftDownRpm : idle + 0.12f * range;
    const float lowest = idle + 0.05f * range;
    points.downClosed = std::min(std::max(points.downClosed, lowest), std::max(points.upFull - 0.3f * range, lowest));
    points.upLight = std::min(std::max(idle + 0.35f * range, points.downClosed + 0.15f * range), points.upFull);
    points.downFull = std::max(std::min(idle + 0.4f * range, points.upFull - 0.35f * range), points.downClosed);
    return points;
}

float VehicleGearRpm(const VehicleGearbox& gearbox, int gear, float outputRpm)
{
    if (gear < 0)
    {
        return std::abs(outputRpm * gearbox.reverseRatio);
    }
    if (gear == 0 || gear > static_cast<int>(gearbox.forwardRatios.size()))
    {
        return 0.0f;
    }
    return std::abs(outputRpm * gearbox.forwardRatios[static_cast<size_t>(gear - 1)]);
}

void UpdateAutomaticGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, float forward, float outputRpm, float deltaSeconds)
{
    const int top = static_cast<int>(gearbox.forwardRatios.size());
    const float throttle = std::clamp(std::abs(forward), 0.0f, 1.0f);
    const auto gearRpm = [&](int gear)
    {
        return VehicleGearRpm(gearbox, gear, outputRpm);
    };
    const auto startChange = [&](bool underLoad)
    {
        // Under load the clutch opens while the gears change, then bites; between gears with no
        // torque through them (from rest, or rolling below the idle) the box just selects it.
        state.switchLeft = underLoad ? gearbox.switchSeconds : 0.0f;
        state.releaseLeft = gearbox.releaseSeconds;
        state.latencyLeft = gearbox.latencySeconds;
    };

    // Drive or reverse, as the throttle asks; the caller only asks once the car has stopped.
    if ((forward > 0.0f && state.gear < 0) || (forward < 0.0f && state.gear > 0) || state.gear == 0)
    {
        state.gear = forward < 0.0f ? -1 : 1;
        startChange(false);
    }

    const bool ready = state.idling || (state.switchLeft <= 0.0f && state.releaseLeft <= 0.0f && state.latencyLeft <= 0.0f);
    if (state.gear > 0 && top > 0 && ready)
    {
        const VehicleShiftPoints& points = gearbox.shiftPoints;
        const float up = points.upLight + (points.upFull - points.upLight) * throttle;
        const float down = points.downClosed + (points.downFull - points.downClosed) * throttle;
        int target = std::min(state.gear, top);
        if (gearRpm(target) > up)
        {
            // Up while the engine is past the point, as long as the next gear keeps it clear of the
            // point it would change back down at.
            while (target < top && gearRpm(target) > up && gearRpm(target + 1) > down * 1.05f)
            {
                ++target;
            }
        }
        else if (gearRpm(target) < down)
        {
            // Down as far as the speed allows without the lower gear passing the point it would change back up at.
            while (target > 1 && gearRpm(target) < down && gearRpm(target - 1) < up * 0.9f)
            {
                --target;
            }
        }
        if (target != state.gear)
        {
            state.gear = target;
            if (!state.idling)
            {
                startChange(true);
            }
        }
    }

    // Off the throttle and too slow for the gear to turn the engine at its idle, the clutch opens: the
    // car rolls to a stop with the engine idling rather than the engine pushing it on.
    const bool idle = throttle <= 0.0f && gearRpm(state.gear) < gearbox.idleRpm;
    if (idle)
    {
        state.idling = true;
        state.switchLeft = 0.0f;
        state.releaseLeft = 0.0f;
        state.latencyLeft = 0.0f;
        state.clutch = 0.0f;
        state.revMatch = false;
        return;
    }
    if (state.idling)
    {
        state.idling = false;
        startChange(false);
    }

    state.revMatch = state.switchLeft > 0.0f;
    if (state.switchLeft > 0.0f)
    {
        state.switchLeft = std::max(state.switchLeft - deltaSeconds, 0.0f);
        state.clutch = 0.0f;
    }
    else if (state.releaseLeft > 0.0f)
    {
        state.releaseLeft = std::max(state.releaseLeft - deltaSeconds, 0.0f);
        state.clutch = gearbox.releaseSeconds > 0.0f ? 1.0f - state.releaseLeft / gearbox.releaseSeconds : 1.0f;
    }
    else
    {
        state.clutch = 1.0f;
        state.latencyLeft = std::max(state.latencyLeft - deltaSeconds, 0.0f);
    }
}
}
