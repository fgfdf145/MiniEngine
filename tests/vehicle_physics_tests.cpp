#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A road car's bounds: 1.8 m wide, 1.4 m tall, 4.4 m long, standing on y = 0.
constexpr glm::vec3 kCarMin(-0.9f, 0.0f, -2.2f);
constexpr glm::vec3 kCarMax(0.9f, 1.4f, 2.2f);

void TestFitPlacesWheelsInsideTheBounds()
{
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    Require(settings.frontAxleZ > 0.8f && settings.frontAxleZ + settings.wheelRadius <= kCarMax.z, "front axle sits inside the nose");
    Require(settings.rearAxleZ < -0.8f && settings.rearAxleZ - settings.wheelRadius >= kCarMin.z, "rear axle sits inside the tail");
    Require(settings.trackCenterX + settings.halfTrackWidth + settings.wheelWidth * 0.5f <= kCarMax.x + 1e-4f, "wheels stay inside the body's width");
    Require(settings.wheelRadius > 0.25f && settings.wheelRadius < 0.4f, "a road car's wheel radius");

    // Settled on its springs, the contact patch is on the floor.
    const float rest = ComputeRestSuspensionLength(settings, 9.81f);
    const float contactY = settings.wheelMountY - rest - settings.wheelRadius;
    Require(std::abs(contactY - kCarMin.y) < 1e-4f, "the tyres rest on the bounds' floor");

    // The chassis clears the ground and the centre of mass sits low.
    Require(settings.chassisCenter.y - settings.chassisHalfExtents.y > kCarMin.y + 0.1f, "the chassis clears the ground");
    const float centerOfMassY = settings.chassisCenter.y + settings.centerOfMassOffset.y;
    Require(centerOfMassY > kCarMin.y && centerOfMassY < kCarMin.y + 0.6f, "the centre of mass sits low");
}

void TestFitFollowsOffCentreBounds()
{
    const glm::vec3 offset(3.0f, 1.0f, -2.0f);
    const VehicleSettings centred = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const VehicleSettings moved = FitVehicleSettingsToBounds(kCarMin + offset, kCarMax + offset);
    Require(std::abs(moved.trackCenterX - (centred.trackCenterX + offset.x)) < 1e-4f, "track follows the bounds in X");
    Require(std::abs(moved.frontAxleZ - (centred.frontAxleZ + offset.z)) < 1e-4f, "axles follow the bounds in Z");
    Require(std::abs(moved.wheelMountY - (centred.wheelMountY + offset.y)) < 1e-4f, "ride height follows the bounds in Y");
}

void TestFitSurvivesFlatBounds()
{
    const VehicleSettings settings = FitVehicleSettingsToBounds(glm::vec3(0.0f), glm::vec3(0.0f));
    Require(settings.wheelRadius > 0.0f && settings.halfTrackWidth > 0.0f, "a flat model still gets wheels");
    Require(settings.chassisHalfExtents.x > 0.0f && settings.chassisHalfExtents.y > 0.0f && settings.chassisHalfExtents.z > 0.0f, "and a chassis");
    Require(settings.frontAxleZ > settings.rearAxleZ, "with the front axle ahead of the rear");
}

void TestFitKeepsTuning()
{
    VehicleSettings tuning;
    tuning.massKg = 900.0f;
    tuning.maxEngineTorque = 250.0f;
    tuning.drive = VehicleDrive::AllWheel;
    tuning.suspensionFrequencyHz = 3.0f;
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    Require(settings.massKg == 900.0f && settings.maxEngineTorque == 250.0f && settings.drive == VehicleDrive::AllWheel, "tuning is kept");
    Require(settings.suspensionFrequencyHz == 3.0f, "the spring rate is kept");

    // A stiffer spring sags less, and the mount moves so the tyres still meet the floor.
    const float contactY = settings.wheelMountY - ComputeRestSuspensionLength(settings, 9.81f) - settings.wheelRadius;
    Require(std::abs(contactY - kCarMin.y) < 1e-4f, "a stiff car's tyres rest on the bounds' floor");
}

void TestDriverInputBrakesBeforeReversing()
{
    float direction = 1.0f;
    VehicleControls controls;
    controls.throttle = -1.0f;

    // Rolling forward: reverse throttle brakes and keeps the gearbox in drive.
    VehicleDriverInput input = ResolveVehicleDriverInput(controls, 10.0f, direction);
    Require(input.forward == 0.0f && input.brake == 1.0f && direction == 1.0f, "reverse throttle at speed brakes");

    // Stopped: now it reverses.
    input = ResolveVehicleDriverInput(controls, 0.1f, direction);
    Require(input.forward == -1.0f && input.brake == 0.0f && direction == -1.0f, "reverse throttle at rest reverses");

    // Rolling backwards: forward throttle brakes first.
    controls.throttle = 0.5f;
    input = ResolveVehicleDriverInput(controls, -3.0f, direction);
    Require(input.forward == 0.0f && input.brake == 0.5f && direction == -1.0f, "forward throttle while reversing brakes");

    // The hand brake cuts the throttle.
    direction = 1.0f;
    controls.throttle = 1.0f;
    controls.handBrake = 1.0f;
    input = ResolveVehicleDriverInput(controls, 0.0f, direction);
    Require(input.forward == 0.0f && input.handBrake == 1.0f, "the hand brake cuts the throttle");
}

void AddGroundMesh(PhysicsWorld& world)
{
    // A 400 m square facing up (counter-clockwise seen from above), as a triangle mesh like a track's.
    const std::vector<glm::vec3> vertices = {
        {-200.0f, 0.0f, -200.0f},
        {-200.0f, 0.0f, 200.0f},
        {200.0f, 0.0f, 200.0f},
        {200.0f, 0.0f, -200.0f},
    };
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    Require(world.AddStaticMesh(vertices, indices), "the ground mesh builds");
    Require(world.GetStaticTriangleCount() == 2, "the ground has two triangles");
}

void Simulate(PhysicsWorld& world, float seconds)
{
    constexpr float kFrame = 1.0f / 144.0f; // not a multiple of the physics step, as a real frame rate
    for (float time = 0.0f; time < seconds; time += kFrame)
    {
        world.Update(kFrame);
    }
}

void TestCarSettlesOnTheGround()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.3f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});

    Simulate(world, 3.0f);
    const PhysicsPose pose = world.GetVehiclePose(car);
    const VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.wheelsInContact == 4, "all four wheels touch the ground, got " + std::to_string(telemetry.wheelsInContact));
    Require(std::abs(pose.position.y) < 0.08f, "the car settles at its ride height, y = " + std::to_string(pose.position.y));
    Require(std::abs(pose.position.x) < 0.05f && std::abs(pose.position.z) < 0.05f, "and stays put");
    Require(world.GetVehicleWheels(car).size() == 4, "four wheels");
}

void TestCarDrivesSteersAndReverses()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const PhysicsPose start{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    const VehicleId car = world.AddVehicle(settings, start);
    Simulate(world, 1.0f);

    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 4.0f);
    VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    PhysicsPose pose = world.GetVehiclePose(car);
    Require(telemetry.forwardSpeed > 8.0f, "full throttle accelerates, speed " + std::to_string(telemetry.forwardSpeed));
    Require(pose.position.z > 15.0f, "the car drives along +Z, z = " + std::to_string(pose.position.z));
    Require(std::abs(pose.position.x) < 1.0f, "and straight, x = " + std::to_string(pose.position.x));
    Require(telemetry.gear >= 1, "in a forward gear");

    // Steering right turns towards -X (+X is the car's left).
    controls.steering = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 1.5f);
    pose = world.GetVehiclePose(car);
    const glm::vec3 forward = pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    Require(forward.x < -0.2f, "steering right turns right, forward.x = " + std::to_string(forward.x));
    Require(std::abs(forward.y) < 0.2f, "without rolling over");

    // Reset puts it back, stopped.
    world.ResetVehicle(car, start);
    pose = world.GetVehiclePose(car);
    Require(glm::length(pose.position - start.position) < 1e-4f, "reset moves the car back");
    Require(std::abs(world.GetVehicleTelemetry(car).forwardSpeed) < 1e-4f, "and stops it");
    Simulate(world, 1.0f);

    // Reverse from rest.
    controls = {};
    controls.throttle = -1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    telemetry = world.GetVehicleTelemetry(car);
    pose = world.GetVehiclePose(car);
    Require(telemetry.forwardSpeed < -2.0f, "reverse throttle backs up, speed " + std::to_string(telemetry.forwardSpeed));
    Require(pose.position.z < -2.0f, "along -Z, z = " + std::to_string(pose.position.z));
    Require(telemetry.gear < 0, "in reverse gear");

    // Braking to a stop.
    controls = {};
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    Require(std::abs(world.GetVehicleTelemetry(car).forwardSpeed) < 0.2f, "the brake stops the car");
}

void TestCarRotatedAtStartDrivesItsOwnWay()
{
    PhysicsWorld world;
    world.AddStaticBox(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(200.0f, 0.5f, 200.0f));
    // Facing +X: a quarter turn about +Y takes +Z to +X.
    const glm::quat facingX = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax), {glm::vec3(0.0f), facingX});
    Simulate(world, 0.5f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    const PhysicsPose pose = world.GetVehiclePose(car);
    Require(pose.position.x > 8.0f && std::abs(pose.position.z) < 1.0f, "a car facing +X drives along +X");
}

void TestUpdateRunsFixedSteps()
{
    PhysicsWorld world;
    Require(world.Update(PhysicsWorld::kFixedStepSeconds * 0.5f) == 0, "half a step runs none");
    Require(world.Update(PhysicsWorld::kFixedStepSeconds * 0.6f) == 1, "the remainder carries over");
    Require(world.Update(10.0f) == PhysicsWorld::kMaxStepsPerUpdate, "a long frame is capped");
    Require(world.Update(0.0f) == 1, "the capped backlog keeps at most one step");
    Require(world.Update(0.0f) == 0, "nothing is left over");
}

void TestDegenerateMeshIsRejected()
{
    PhysicsWorld world;
    const std::vector<glm::vec3> vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    Require(!world.AddStaticMesh(vertices, std::vector<uint32_t>{0, 0, 1}), "a triangle repeating a vertex is dropped");
    Require(!world.AddStaticMesh(vertices, std::vector<uint32_t>{0, 1, 7}), "an index out of range is dropped");
    Require(world.GetStaticBodyCount() == 0, "and nothing is added");
}
}

int main()
{
    try
    {
        TestFitPlacesWheelsInsideTheBounds();
        TestFitFollowsOffCentreBounds();
        TestFitSurvivesFlatBounds();
        TestFitKeepsTuning();
        TestDriverInputBrakesBeforeReversing();
        TestUpdateRunsFixedSteps();
        TestDegenerateMeshIsRejected();
        TestCarSettlesOnTheGround();
        TestCarDrivesSteersAndReverses();
        TestCarRotatedAtStartDrivesItsOwnWay();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "vehicle physics tests failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "vehicle physics tests passed\n";
    return 0;
}
