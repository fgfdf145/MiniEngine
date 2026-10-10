#include "gtr_car_spec.h"

#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// A car put back with ResetVehicle is the car as it was made: the same controls drive it along the same
// line, step for step, whatever it did before. A replayed drive (VehicleDriveService::StartReplay) gives
// the car only the recording's controls, so anything the reset leaves behind takes it off the line.
using namespace me;
using me::test::MakeGtrSpec;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void AddGround(PhysicsWorld& world)
{
    const std::vector<glm::vec3> vertices = {{-400.0f, 0.0f, -400.0f}, {-400.0f, 0.0f, 400.0f}, {400.0f, 0.0f, 400.0f}, {400.0f, 0.0f, -400.0f}};
    Require(world.AddStaticMesh(vertices, std::vector<uint32_t>{0, 1, 2, 0, 2, 3}), "the ground builds");
}

// The tyre of the tyre temperature test: the game's thermal and wear data.
VehicleTyreSettings ThermalTyre()
{
    VehicleTyreSettings tyre{1.30f, 1.28f, 0.13f, 7.62f, 0.86f, 1.36f};
    tyre.lateralReference = 1.28f;
    tyre.referenceLoad = 2860.0f;
    tyre.lateralLoadExponent = 0.8334f;
    tyre.pressureGripGain = 0.0045f;
    tyre.pressureSpringGain = 8111.0f;
    tyre.thermal.surfaceTransfer = 0.0150;
    tyre.thermal.patchTransfer = 0.00027;
    tyre.thermal.coreTransfer = 0.00015;
    tyre.thermal.internalCoreTransfer = 0.0029;
    tyre.thermal.frictionK = 0.06446;
    tyre.thermal.rollingK = 0.18;
    tyre.thermal.surfaceRollingK = 0.96443;
    tyre.thermal.coolFactor = 2.17;
    tyre.thermal.performanceCurve = {{0, 0.8f}, {20, 0.92f}, {40, 0.95f}, {60, 0.98f}, {75, 1.0f}, {95, 1.0f}, {105, 0.97f}};
    tyre.thermal.staticPressure = 28.0;
    tyre.thermal.idealPressure = 33.0;
    tyre.thermal.rollingResistanceGain = 0.55;
    tyre.wear.wearCurve = {{0.0f, 100.0f}, {1.25f, 99.5f}, {10.0f, 98.0f}, {25.0f, 80.0f}};
    tyre.wear.useLoad = true;
    tyre.wear.referenceLoad = 2860.0;
    tyre.wear.grainGain = 0.4;
    tyre.wear.grainGamma = 1.0;
    tyre.wear.blisterGain = 0.3;
    tyre.wear.blisterGamma = 1.0;
    tyre.wear.performanceCurve = tyre.thermal.performanceCurve;
    return tyre;
}

// The GT-R on its multibody suspension with hub masses, brush tyres warming and wearing, ABS and traction
// control on: every state the car carries from step to step.
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
    tuning.brushTyreRibs = 8;
    tuning.brushTyreSegments = 8;
    SetAxleTyres(tuning, true, ThermalTyre());
    SetAxleTyres(tuning, false, ThermalTyre());
    tuning.tyreTemperatures = true;
    tuning.tyreWear = true;
    VehicleSettings settings = FitVehicleSettingsToBounds(glm::vec3(-1.0f, 0.0f, -2.3f), glm::vec3(1.0f, 1.2f, 2.3f), tuning, &layout);
    settings.centerOfMassOffset = glm::vec3(0.0f, 0.38f - settings.chassisCenter.y, -settings.chassisCenter.z);
    return settings;
}

constexpr int kStepsPerFrame = 16; // a 60 Hz frame at the world's 1000 Hz, near enough

// A slalom from a standing start: throttle, steering back and forth, a stop with the brakes on hard.
VehicleControls Script(int frame)
{
    const float seconds = static_cast<float>(frame * kStepsPerFrame) * 0.001f;
    VehicleControls controls;
    if (seconds < 7.0f)
    {
        controls.throttle = 0.7f;
        controls.steering = 0.5f * std::sin(seconds * 1.6f);
    }
    else
    {
        controls.brake = 1.0f;
        controls.steering = 0.3f;
    }
    return controls;
}

constexpr int kScriptFrames = 600; // 9.6 s

// The body's position after each frame of the script, driven from `start`.
std::vector<glm::dvec3> DriveScript(PhysicsWorld& world, VehicleId car)
{
    std::vector<glm::dvec3> positions;
    for (int frame = 0; frame < kScriptFrames; ++frame)
    {
        world.SetVehicleControls(car, Script(frame));
        world.RunSteps(kStepsPerFrame);
        positions.push_back(world.GetVehiclePose(car).position);
    }
    return positions;
}

double LargestGap(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    double gap = 0.0;
    for (size_t index = 0; index < a.size() && index < b.size(); ++index)
    {
        gap = std::max(gap, glm::length(a[index] - b[index]));
    }
    return gap;
}

const PhysicsPose kStart{glm::dvec3(0.0, 0.3, -100.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};

// As the editor starts a drive: the car is made, then put at its start.
std::vector<glm::dvec3> DriveFresh()
{
    PhysicsWorld world;
    AddGround(world);
    const VehicleId car = world.AddVehicle(Gtr(), {glm::dvec3(0.0, 0.3, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    world.ResetVehicle(car, kStart);
    return DriveScript(world, car);
}

// A car that has been driven hard (warm worn tyres, a high gear, ABS and traction control busy, the
// suspension loaded) put back at the start drives the script as a new one does.
void TestResetCarDrivesAsNew()
{
    const std::vector<glm::dvec3> fresh = DriveFresh();
    const std::vector<glm::dvec3> again = DriveFresh();
    Require(LargestGap(fresh, again) == 0.0, "two new cars drive the script alike");
    const glm::dvec3 end = fresh.back();
    std::cout << "the script ends at (" << end.x << ", " << end.z << "), "
              << glm::length(glm::dvec2(end.x - kStart.position.x, end.z - kStart.position.z)) << " m from the start\n";

    PhysicsWorld world;
    AddGround(world);
    const VehicleId car = world.AddVehicle(Gtr(), {glm::dvec3(0.0, 0.3, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    world.ResetVehicle(car, kStart);
    // Flat out in circles, then hard on the brakes mid-turn, stopping where the script starts.
    for (int frame = 0; frame < 900; ++frame)
    {
        VehicleControls controls;
        if (frame < 700)
        {
            controls.throttle = 1.0f;
            controls.steering = 0.8f;
        }
        else
        {
            controls.brake = 1.0f;
            controls.steering = -0.6f;
        }
        world.SetVehicleControls(car, controls);
        world.RunSteps(kStepsPerFrame);
    }
    const VehicleTelemetry driven = world.GetVehicleTelemetry(car);
    std::cout << "after the hard drive: gear " << driven.gear << ", front left tread " << driven.tyres[0].tread[1] << " C\n";
    world.ResetVehicle(car, kStart);
    const std::vector<glm::dvec3> reset = DriveScript(world, car);
    const double gap = LargestGap(fresh, reset);
    std::cout << "a reset car off a new one's line by " << gap << " m at most\n";
    Require(gap == 0.0, "the reset car drives the script as a new one, off by " + std::to_string(gap) + " m");
}
}

int main()
{
    try
    {
        TestResetCarDrivesAsNew();
        std::cout << "[pass] a reset car drives as new\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "[FAIL] a reset car drives as new: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
