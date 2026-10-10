#include "gtr_car_spec.h"

#include <engine/core/threading/task_system.h>
#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Cars on the brush tyre with a flexible carcass: they stand, launch, brake, corner and park on a slope
// as cars do.
using namespace me;
using me::test::MakeGtrSpec;

namespace
{
constexpr float kPi = 3.14159265358979f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Finite(const glm::dvec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A 400 m square, tilted up towards +Z by `slopeDegrees`.
void AddGround(PhysicsWorld& world, float slopeDegrees = 0.0f, float friction = PhysicsWorld::kDefaultSurfaceFriction)
{
    const float rise = std::tan(slopeDegrees * kPi / 180.0f) * 200.0f;
    const std::vector<glm::vec3> vertices = {
        {-200.0f, -rise, -200.0f},
        {-200.0f, rise, 200.0f},
        {200.0f, rise, 200.0f},
        {200.0f, -rise, -200.0f},
    };
    Require(world.AddStaticMesh(vertices, std::vector<uint32_t>{0, 1, 2, 0, 2, 3}, friction), "the ground builds");
}

void Simulate(PhysicsWorld& world, float seconds)
{
    constexpr float kFrame = 1.0f / 144.0f;
    for (float time = 0.0f; time < seconds; time += kFrame)
    {
        world.Update(kFrame);
    }
}

// The Boxster of the vehicle physics tests: straight springs, its own engine, gearbox and tyre figures.
VehicleCarSpec MakeBoxsterSpec()
{
    VehicleCarSpec spec;
    spec.massKg = 1460.0f;
    spec.drive = VehicleDrive::RearWheel;
    spec.torqueCurve = {{500.0f, 110.0f}, {1000.0f, 200.0f}, {1500.0f, 297.0f}, {2000.0f, 386.0f}, {4500.0f, 386.0f}, {5500.0f, 366.0f}, {6500.0f, 346.0f}, {7500.0f, 267.0f}, {8500.0f, 0.0f}};
    spec.minRpm = 900.0f;
    spec.maxRpm = 7500.0f;
    spec.gearRatios = {3.91f, 2.29f, 1.65f, 1.30f, 1.08f, 0.88f, 0.62f};
    spec.reverseGearRatio = -3.55f;
    spec.finalDriveRatio = 3.62f;
    spec.gearSwitchSeconds = 0.03f;
    spec.clutchReleaseSeconds = 0.1f;
    spec.engineInertia = 0.137f;
    spec.frontTyres = VehicleTyreSettings{1.314f, 1.303f, 0.13f, 7.52f, 0.86f, 1.62f};
    spec.rearTyres = VehicleTyreSettings{1.294f, 1.271f, 0.128f, 7.27f, 0.86f, 1.97f};
    spec.maxSteerAngleDegrees = 26.7f;
    spec.brakeTorquePerWheel = 800.0f;
    spec.frontBrakeShare = 0.65f;
    spec.handBrakeTorquePerWheel = 1000.0f;
    spec.suspensionFrequencyHz = 1.7f;
    spec.suspensionDamping = 0.7f;
    spec.antiRollBars = true;
    spec.limitedSlipDifferentials = true;
    return spec;
}

VehicleSettings Boxster()
{
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    return FitVehicleSettingsToBounds(glm::vec3(-0.9f, 0.0f, -2.2f), glm::vec3(0.9f, 1.4f, 2.2f), tuning);
}

// The GT-R on its multibody suspension with hub masses on tyre springs (see the vehicle physics tests).
VehicleSettings Gtr()
{
    const VehicleCarSpec spec = MakeGtrSpec();
    VehicleWheelLayout layout{};
    const float frontZ = 2.78f * (1.0f - 0.555f);
    const float rearZ = frontZ - 2.78f;
    layout[0] = {glm::vec3(0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8375f, 0.355f, frontZ), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.84f, 0.355f, rearZ), 0.355f, 0.33f};
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, spec);
    tuning.wheelRadius = 0.355f;
    VehicleSettings settings = FitVehicleSettingsToBounds(glm::vec3(-1.0f, 0.0f, -2.3f), glm::vec3(1.0f, 1.2f, 2.3f), tuning, &layout);
    settings.centerOfMassOffset = glm::vec3(0.0f, 0.38f - settings.chassisCenter.y, -settings.chassisCenter.z);
    return settings;
}

float Heading(const glm::quat& rotation)
{
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    return std::atan2(forward.x, forward.z);
}

// ---- Standing ----

// Dropped onto flat ground, the car settles on its four tyres and then stays put: no creep, no shimmy.
void TestCarStandsStill(const char* car, const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 3.0f);
    const glm::dvec3 settled = world.GetVehiclePose(id).position;
    Simulate(world, 5.0f);
    const glm::dvec3 later = world.GetVehiclePose(id).position;
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(id);
    std::cout << car << " standing: moved " << glm::length(later - settled) * 1000.0f << " mm in 5 s, carcass FL " << wheels[0].carcassDeflection.x * 1000.0f
              << " / " << wheels[0].carcassDeflection.y * 1000.0f << " mm\n";
    Require(Finite(later), "the car's position stays finite");
    Require(world.GetVehicleTelemetry(id).wheelsInContact == 4, "all four tyres on the ground");
    Require(glm::length(later - settled) < 0.01f, std::string(car) + " stands still on flat ground");
}

// On a 10 degree slope with the hand brake and the brakes on, the car holds. The brush tyre's bristles
// keep their bend standing still, so once its tyres have taken up the pull it does not creep at all.
void TestCarHoldsOnASlope(const char* car, const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world, 10.0f);
    const float y = std::tan(10.0f * kPi / 180.0f) * 0.0f;
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, y + 0.1f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.brake = 1.0f;
    controls.handBrake = 1.0f;
    world.SetVehicleControls(id, controls);
    Simulate(world, 2.0f);
    const glm::dvec3 start = world.GetVehiclePose(id).position;
    Simulate(world, 2.5f);
    const glm::dvec3 middle = world.GetVehiclePose(id).position;
    const std::vector<VehicleWheelState> before = world.GetVehicleWheels(id);
    Simulate(world, 2.5f);
    const std::vector<VehicleWheelState> after = world.GetVehicleWheels(id);
    const float slid = glm::length(world.GetVehiclePose(id).position - start);
    const float late = glm::length(world.GetVehiclePose(id).position - middle);
    // The braked wheels stand still while their tyres hold the car: drawn by their roll angle, they
    // turned on at several degrees a second when the angle took the spin from before the brakes.
    float turned = 0.0f;
    for (size_t index = 0; index < after.size(); ++index)
    {
        turned = std::max(turned, std::abs(after[index].spinAngle - before[index].spinAngle));
    }
    std::cout << car << " on a 10 deg slope, braked: slid " << slid * 1000.0f << " mm in 5 s, "
              << late * 1000.0f << " mm of it in the last 2.5 s, a wheel turned " << turned * 180.0f / kPi << " deg in it\n";
    Require(turned < 0.2f * kPi / 180.0f, std::string(car) + "'s braked wheels stand still, one turned " + std::to_string(turned * 180.0f / kPi) + " deg");
    Require(Finite(world.GetVehiclePose(id).position), "the car's position stays finite");
    Require(slid < 0.05f, std::string(car) + " holds on the slope");
    Require(late < 1e-4f, std::string(car) + " does not creep, moved " + std::to_string(late * 1000.0f) + " mm");
}

// Rolled gently and let go on flat ground, the car comes to rest on its rolling resistance alone and
// stays there, its wheels still: the resistance holds a wheel at rest as dry friction does. Smoothed
// through the standstill (a drag in proportion to the spin below 0.5 rad/s) it let the car roll on at
// millimetres a second for a minute, its wheels turning while it looked parked.
void TestCarRollsToAStop(const char* car, const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 2.0f);
    VehicleControls controls;
    controls.throttle = 0.3f;
    world.SetVehicleControls(id, controls);
    for (int i = 0; i < 500 && world.GetVehicleTelemetry(id).forwardSpeed < 0.5f; ++i)
    {
        Simulate(world, 0.01f);
    }
    const float rolling = world.GetVehicleTelemetry(id).forwardSpeed;
    world.SetVehicleControls(id, VehicleControls{});
    // At 1.2 % of its weight the resistance takes some 4.5 s to stop a car from 0.5 m/s.
    Simulate(world, 8.0f);
    const glm::dvec3 settled = world.GetVehiclePose(id).position;
    const std::vector<VehicleWheelState> before = world.GetVehicleWheels(id);
    Simulate(world, 5.0f);
    const std::vector<VehicleWheelState> after = world.GetVehicleWheels(id);
    const float moved = glm::length(world.GetVehiclePose(id).position - settled);
    float turned = 0.0f;
    for (size_t index = 0; index < after.size(); ++index)
    {
        turned = std::max(turned, std::abs(after[index].spinAngle - before[index].spinAngle));
    }
    std::cout << car << " let go at " << rolling << " m/s: in the 5 s from 8 s after, moved " << moved * 1000.0f
              << " mm, a wheel turned " << turned * 180.0f / kPi << " deg\n";
    Require(rolling > 0.3f, std::string(car) + " rolls before it is let go");
    Require(moved < 1e-3f, std::string(car) + " comes to rest, moved " + std::to_string(moved * 1000.0f) + " mm");
    Require(turned < 0.2f * kPi / 180.0f, std::string(car) + "'s wheels stop turning, one turned " + std::to_string(turned * 180.0f / kPi) + " deg");
}

// Far from the world's origin the car drives as it does at it: let go at 0.5 m/s 6 km out (where the
// GTA map's Vice City lies), it rolls as far before it stops. With the world in float, positions there
// are 0.5 mm apart and a 1 ms step under 0.24 m/s moved the car not at all, so it stopped short and
// then stood with its wheels still turning.
void TestFarFromTheOriginAsAtIt(const char* car, const VehicleSettings& settings)
{
    const auto rollOut = [&](const glm::dvec3& origin)
    {
        PhysicsWorld world;
        const std::vector<glm::vec3> vertices = {{-200.0f, 0.0f, -200.0f}, {-200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, -200.0f}};
        Require(world.AddStaticMesh(vertices, std::vector<uint32_t>{0, 1, 2, 0, 2, 3}, SurfaceGrip{}, origin), "the ground builds");
        const VehicleId id = world.AddVehicle(settings, {origin + glm::dvec3(0.0, 0.1, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 2.0f);
        VehicleControls controls;
        controls.throttle = 0.3f;
        world.SetVehicleControls(id, controls);
        for (int i = 0; i < 500 && world.GetVehicleTelemetry(id).forwardSpeed < 0.5f; ++i)
        {
            Simulate(world, 0.01f);
        }
        world.SetVehicleControls(id, VehicleControls{});
        const glm::dvec3 released = world.GetVehiclePose(id).position;
        Simulate(world, 8.0f);
        return std::pair{glm::length(world.GetVehiclePose(id).position - released), released - origin};
    };
    const auto [atOrigin, launchAtOrigin] = rollOut(glm::dvec3(0.0));
    const auto [farOut, launchFarOut] = rollOut(glm::dvec3(6000.0, 6.0, -6000.0));
    std::cout << car << " let go at 0.5 m/s: rolls " << atOrigin * 1000.0 << " mm at the origin, " << farOut * 1000.0
              << " mm 6 km out\n";
    Require(atOrigin > 0.3, std::string(car) + " rolls on after it is let go");
    Require(glm::length(launchFarOut - launchAtOrigin) < 1e-3, std::string(car) + " sets off as it does at the origin");
    Require(std::abs(farOut - atOrigin) < 1e-3, std::string(car) + " rolls as far 6 km out as at the origin");
}

// ---- Load ----

// The brush tyre works with the load the suspension is about to push with in the step, not the last
// step's: as the brakes go on at speed and the load moves forward, what each tyre took is nearer what the
// physics engine's solver then pushed with than the step before's push is.
void TestTyreLoadIsTheSteps(const char* car, const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    Simulate(world, 3.0f);
    controls.throttle = 0.0f;
    controls.brake = 1.0f;
    world.SetVehicleControls(id, controls);
    double foreseen = 0.0;
    double stepOld = 0.0;
    double total = 0.0;
    std::vector<VehicleWheelState> before = world.GetVehicleWheels(id);
    for (int step = 0; step < 400; ++step)
    {
        world.Update(PhysicsWorld::kDefaultStepSeconds);
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(id);
        for (size_t index = 0; index < wheels.size(); ++index)
        {
            if (!wheels[index].inContact || !before[index].inContact)
            {
                continue;
            }
            foreseen += std::abs(wheels[index].tyreLoad - wheels[index].suspensionForce);
            stepOld += std::abs(before[index].suspensionForce - wheels[index].suspensionForce);
            total += wheels[index].suspensionForce;
        }
        before = wheels;
    }
    std::cout << car << " braking from speed: the tyre's load off the solver's push by " << 100.0 * foreseen / total << " % on average, the step before's by "
              << 100.0 * stepOld / total << " %\n";
    Require(foreseen < 0.5 * stepOld, std::string(car) + " foresees the step's load better than the step before's push does");
}

// ---- Driving ----

float TimeTo100(const VehicleSettings& settings, int& gear)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    constexpr float kFrame = 1.0f / 144.0f;
    gear = 0;
    for (float time = 0.0f; time < 20.0f; time += kFrame)
    {
        world.Update(kFrame);
        const VehicleTelemetry telemetry = world.GetVehicleTelemetry(id);
        gear = std::max(gear, telemetry.gear);
        if (telemetry.forwardSpeed >= 100.0f / 3.6f)
        {
            return time;
        }
    }
    return 20.0f;
}

void TestCarLaunches(const char* car, const VehicleSettings& settings)
{
    int gear = 0;
    const float time = TimeTo100(settings, gear);
    std::cout << car << " 0-100 km/h: " << time << " s (gear " << gear << ")\n";
    Require(time > 2.5f && time < 9.0f, std::string(car) + " reaches 100 km/h in a sports car's time, got " + std::to_string(time));
    // Up at least once: the GT-R's second runs on to 137 km/h, so 100 km/h comes in second.
    Require(gear >= 2, "it changes up");
}

struct StopReport
{
    float distance = 0.0f;
    float peakDeceleration = 0.0f;
    float lockedShare = 0.0f;
    float drift = 0.0f; // sideways, m
};

StopReport StopFrom100(const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    for (int i = 0; i < 2000 && world.GetVehicleTelemetry(id).forwardSpeed < 100.0f / 3.6f; ++i)
    {
        Simulate(world, 0.01f);
    }
    controls = {};
    controls.brake = 1.0f;
    world.SetVehicleControls(id, controls);
    StopReport report;
    const glm::dvec3 start = world.GetVehiclePose(id).position;
    float previous = world.GetVehicleTelemetry(id).forwardSpeed;
    int samples = 0;
    int locked = 0;
    for (float time = 0.0f; time < 10.0f; time += 0.01f)
    {
        Simulate(world, 0.01f);
        const float speed = world.GetVehicleTelemetry(id).forwardSpeed;
        report.peakDeceleration = std::max(report.peakDeceleration, (previous - speed) / 0.01f);
        previous = speed;
        if (speed > 5.0f)
        {
            for (const VehicleWheelState& wheel : world.GetVehicleWheels(id))
            {
                ++samples;
                locked += std::abs(wheel.angularVelocity) * wheel.radius < 0.2f * speed ? 1 : 0;
            }
        }
        if (speed < 0.1f)
        {
            break;
        }
    }
    const glm::dvec3 end = world.GetVehiclePose(id).position;
    report.distance = std::abs(end.z - start.z);
    report.drift = std::abs(end.x - start.x);
    report.lockedShare = samples > 0 ? static_cast<float>(locked) / static_cast<float>(samples) : 0.0f;
    return report;
}

void TestCarBrakes(const char* car, const VehicleSettings& settings)
{
    const StopReport b = StopFrom100(settings);
    std::cout << car << " 100-0 km/h: " << b.distance << " m (peak " << b.peakDeceleration / 9.81f << " g, locked " << b.lockedShare * 100.0f << "%, drift " << b.drift
              << " m)\n";
    Require(b.distance > 25.0f && b.distance < 70.0f, std::string(car) + " stops from 100 km/h in a car's distance, got " + std::to_string(b.distance));
    Require(b.drift < 1.0f, "it stops straight");
}

// Steering held at a share of lock at a steady speed: the cornering acceleration (speed times yaw rate)
// once settled, and whether it stays on its line or spins.
struct CornerReport
{
    float lateralG = 0.0f;
    float speed = 0.0f;
    bool spun = false;
};

CornerReport Corner(const VehicleSettings& settings, float speed, float steering)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -150.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    for (int i = 0; i < 3000 && world.GetVehicleTelemetry(id).forwardSpeed < speed; ++i)
    {
        controls.throttle = 1.0f;
        world.SetVehicleControls(id, controls);
        Simulate(world, 0.01f);
    }
    CornerReport report;
    controls.steering = steering;
    float heading = Heading(world.GetVehiclePose(id).rotation);
    float sum = 0.0f;
    int samples = 0;
    for (float time = 0.0f; time < 6.0f; time += 0.02f)
    {
        // Hold the speed with the throttle.
        const float v = world.GetVehicleTelemetry(id).forwardSpeed;
        controls.throttle = std::clamp((speed - v) * 0.5f, 0.0f, 1.0f);
        world.SetVehicleControls(id, controls);
        Simulate(world, 0.02f);
        const float next = Heading(world.GetVehiclePose(id).rotation);
        float turn = next - heading;
        turn -= 2.0f * kPi * std::round(turn / (2.0f * kPi));
        heading = next;
        if (time > 3.0f)
        {
            sum += std::abs(world.GetVehicleTelemetry(id).forwardSpeed * turn / 0.02f);
            ++samples;
        }
        if (world.GetVehicleTelemetry(id).forwardSpeed < 0.3f * speed)
        {
            report.spun = true;
        }
    }
    report.lateralG = samples > 0 ? sum / static_cast<float>(samples) / 9.81f : 0.0f;
    report.speed = world.GetVehicleTelemetry(id).forwardSpeed;
    return report;
}

void TestCarCorners(const char* car, const VehicleSettings& settings)
{
    for (const float steering : {0.1f, 0.3f, 0.6f})
    {
        const CornerReport b = Corner(settings, 20.0f, steering);
        std::cout << car << " at 20 m/s, steering " << steering << ": " << b.lateralG << " g" << (b.spun ? " (spun)" : "") << " at " << b.speed << " m/s\n";
        Require(std::isfinite(b.lateralG), "finite");
        Require(b.lateralG < 2.0f, "no more grip than the tyres have");
    }
    const CornerReport gentle = Corner(settings, 20.0f, 0.1f);
    Require(gentle.lateralG > 0.1f && !gentle.spun, std::string(car) + " turns on a little steering");
}

// Full speed down the straight with the wheel centred: it runs straight, no weave.
void TestCarRunsStraight(const char* car, const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    float worstYaw = 0.0f;
    for (float time = 0.0f; time < 10.0f; time += 0.05f)
    {
        Simulate(world, 0.05f);
        worstYaw = std::max(worstYaw, std::abs(Heading(world.GetVehiclePose(id).rotation)));
        if (world.GetVehiclePose(id).position.z > 180.0f)
        {
            break;
        }
    }
    const glm::dvec3 end = world.GetVehiclePose(id).position;
    std::cout << car << " flat out straight: " << world.GetVehicleTelemetry(id).forwardSpeed * 3.6f << " km/h, sideways " << end.x << " m, worst heading " << worstYaw * 180.0f / kPi << " deg\n";
    Require(std::abs(end.x) < 1.0f && worstYaw < 2.0f * kPi / 180.0f, std::string(car) + " runs straight");
}

// What the physics costs a simulated second, flat out down the straight.
void ReportCost(const char* car, const VehicleSettings& brush)
{
    const auto measure = [](const VehicleSettings& settings)
    {
        PhysicsWorld world;
        AddGround(world);
        const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        controls.steering = 0.05f;
        world.SetVehicleControls(id, controls);
        const auto start = std::chrono::steady_clock::now();
        Simulate(world, 5.0f);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / 5.0;
    };
    std::cout << car << " physics per simulated second (1000 steps): " << measure(brush) * 1000.0 << " ms\n";
    // Finer cuts with the engine's task system running, as in the editor: the four tyres step side by side.
    TaskSystem::Initialize();
    for (const auto& [ribs, segments] : {std::pair{10, 20}, std::pair{32, 32}, std::pair{100, 20}, std::pair{64, 64}})
    {
        VehicleSettings cut = brush;
        cut.brushTyreRibs = ribs;
        cut.brushTyreSegments = segments;
        std::cout << car << " brush " << ribs << " x " << segments << " with the task system: " << measure(cut) * 1000.0 << " ms per simulated second\n";
    }
    TaskSystem::Shutdown();
}

// ---- Surfaces ----

// Dry asphalt for z < 0 and the surface under test beyond: the car gets up to speed on the asphalt.
void AddRunUp(PhysicsWorld& world, const SurfaceGrip& surface)
{
    const std::vector<uint32_t> quad{0, 1, 2, 0, 2, 3};
    const std::vector<glm::vec3> asphalt = {{-200.0f, 0.0f, -300.0f}, {-200.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 0.0f}, {200.0f, 0.0f, -300.0f}};
    const std::vector<glm::vec3> test = {{-200.0f, 0.0f, 0.0f}, {-200.0f, 0.0f, 300.0f}, {200.0f, 0.0f, 300.0f}, {200.0f, 0.0f, 0.0f}};
    Require(world.AddStaticMesh(asphalt, quad), "the run-up builds");
    Require(world.AddStaticMesh(test, quad, surface), "the test surface builds");
}

// Up to `speed` on the asphalt, then onto the surface: the mean deceleration over the first `seconds`
// of full braking (or of coasting with `brake` off), in g.
float DecelerationOn(const VehicleSettings& settings, const SurfaceGrip& surface, float speed, bool brake, float seconds)
{
    PhysicsWorld world;
    AddRunUp(world, surface);
    const VehicleId id = world.AddVehicle(settings, {glm::vec3(0.0f, 0.1f, -280.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(id, controls);
    // Held at the speed onto the surface.
    for (int i = 0; i < 6000 && world.GetVehiclePose(id).position.z < 8.0f; ++i)
    {
        controls.throttle = world.GetVehicleTelemetry(id).forwardSpeed < speed ? 1.0f : 0.0f;
        world.SetVehicleControls(id, controls);
        Simulate(world, 0.01f);
    }
    controls = {};
    Require(world.GetVehiclePose(id).position.z >= 8.0f, "the car reaches the test surface");
    controls.brake = brake ? 1.0f : 0.0f;
    world.SetVehicleControls(id, controls);
    const float before = world.GetVehicleTelemetry(id).forwardSpeed;
    Simulate(world, seconds);
    return (before - world.GetVehicleTelemetry(id).forwardSpeed) / seconds / 9.81f;
}

SurfaceGrip Grip(float friction, float cap = 0.0f, float falloff = 0.0f, float sliding = 0.0f, float rolling = 0.0f)
{
    SurfaceGrip grip;
    grip.friction = friction;
    grip.frictionCap = cap;
    grip.wetSpeedFalloff = falloff;
    grip.slidingShare = sliding;
    grip.rollingResistance = rolling;
    return grip;
}

// The test track's surfaces (tools/render_scenes/make_car_test_track.py, after Wong's table): off the
// pavement the ground's own limit holds whatever the tyre; wet asphalt loses grip with speed; soft ground
// drags.
void TestSurfacesGripAsTheGroundDoes(const char* car, VehicleSettings (*make)())
{
    // Without the body's stand-in for air drag (5% of the speed a second, 0.08 g at 15 m/s), so what
    // slows the car is the tyres and the ground; the tyre's own rolling resistance (0.012) remains.
    VehicleSettings brush = make();
    brush.linearDamping = 0.0f;
    const SurfaceGrip ice = Grip(0.12f, 0.1f, 0.0f, 0.7f);
    const SurfaceGrip snow = Grip(0.24f, 0.2f, 0.0f, 0.75f, 0.013f);
    const SurfaceGrip wet = Grip(0.95f, 0.0f, 0.0173f, 0.86f);
    const SurfaceGrip grass = Grip(0.53f, 0.45f, 0.0f, 0.9f, 0.06f);

    const float dry = DecelerationOn(brush, Grip(1.0f), 15.0f, true, 1.0f);
    const float onIce = DecelerationOn(brush, ice, 15.0f, true, 1.0f);
    const float onSnow = DecelerationOn(brush, snow, 15.0f, true, 1.0f);
    std::cout << car << " braking from 15 m/s: dry " << dry << " g, snow " << onSnow << " g, ice " << onIce << " g\n";
    Require(onIce > 0.04f && onIce < 0.1f + 0.02f, std::string(car) + " brakes on ice within its 0.1 cap, got " + std::to_string(onIce));
    Require(onSnow > 0.1f && onSnow < 0.2f + 0.03f, std::string(car) + " brakes on snow within its 0.2 cap, got " + std::to_string(onSnow));
    Require(dry > 0.5f, "dry asphalt still stops it as hard as its brakes do (about 0.65 g from 15 m/s for these two)");

    // Brakes that lock the wheels and no air, so the stop is the wet road's sliding grip alone.
    VehicleSettings locking = brush;
    locking.maxBrakeTorque = 5000.0f;
    locking.aeroSurfaces.clear();
    const float wetSlow = DecelerationOn(locking, wet, 9.0f, true, 0.3f);
    const float wetFast = DecelerationOn(locking, wet, 30.0f, true, 0.3f);
    std::cout << car << " braking on wet asphalt: " << wetSlow << " g at 9 m/s, " << wetFast << " g at 30 m/s\n";
    Require(wetFast < 0.85f * wetSlow, "wet asphalt grips less at speed");

    // Coasting on the capped grass a single run swings by a few hundredths of a g with a tenth of a m/s more
    // or less run-up (measured 2026-10-04: 0.02 to 0.06 g over asphalt between 14.8 and 15.2 m/s), so the
    // drag is the mean of three runs.
    const auto coast = [&](const SurfaceGrip& surface)
    {
        float sum = 0.0f;
        for (const float speed : {14.8f, 15.0f, 15.2f})
        {
            sum += DecelerationOn(brush, surface, speed, false, 2.0f);
        }
        return sum / 3.0f;
    };
    const float coastDry = coast(Grip(1.0f));
    const float coastGrass = coast(grass);
    std::cout << car << " coasting from 15 m/s: asphalt " << coastDry << " g, grass " << coastGrass << " g\n";
    Require(coastGrass - coastDry > 0.03f, "grass drags a coasting car");
}
}

int main()
{
    struct Car
    {
        const char* name;
        VehicleSettings (*make)();
    };
    const Car cars[] = {{"Boxster", Boxster}, {"GT-R", Gtr}};
    int failures = 0;
    const auto run = [&](const std::string& name, const auto& test)
    {
        try
        {
            test();
            std::cout << "[pass] " << name << "\n";
        }
        catch (const std::exception& e)
        {
            ++failures;
            std::cout << "[FAIL] " << name << ": " << e.what() << "\n";
        }
    };
    for (const Car& car : cars)
    {
        const VehicleSettings brush = car.make();
        const std::string prefix = std::string(car.name) + ": ";
        run(prefix + "stands still", [&]
            {
                TestCarStandsStill(car.name, brush);
            });
        run(prefix + "holds on a slope", [&]
            {
                TestCarHoldsOnASlope(car.name, brush);
            });
        run(prefix + "rolls to a stop", [&]
            {
                TestCarRollsToAStop(car.name, brush);
            });
        run(prefix + "far from the origin", [&]
            {
                TestFarFromTheOriginAsAtIt(car.name, brush);
            });
        run(prefix + "tyre load is the step's", [&]
            {
                TestTyreLoadIsTheSteps(car.name, brush);
            });
        run(prefix + "launches", [&]
            {
                TestCarLaunches(car.name, brush);
            });
        run(prefix + "brakes", [&]
            {
                TestCarBrakes(car.name, brush);
            });
        run(prefix + "corners", [&]
            {
                TestCarCorners(car.name, brush);
            });
        run(prefix + "runs straight", [&]
            {
                TestCarRunsStraight(car.name, brush);
            });
        run(prefix + "surfaces", [&]
            {
                TestSurfacesGripAsTheGroundDoes(car.name, car.make);
            });
        run(prefix + "cost", [&]
            {
                ReportCost(car.name, brush);
            });
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
