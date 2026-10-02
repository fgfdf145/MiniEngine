#include <engine/physics/collision_filter.h>
#include <engine/physics/physics_world.h>
#include <engine/physics/vehicle_settings.h>
#include <engine/physics/vehicle_wheel_motion.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
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

void RequireNear(float actual, float expected, float tolerance, const std::string& what)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(what + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected));
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

void AddGroundMesh(PhysicsWorld& world, float friction = PhysicsWorld::kDefaultSurfaceFriction)
{
    // A 400 m square facing up (counter-clockwise seen from above), as a triangle mesh like a track's.
    const std::vector<glm::vec3> vertices = {
        {-200.0f, 0.0f, -200.0f},
        {-200.0f, 0.0f, 200.0f},
        {200.0f, 0.0f, 200.0f},
        {200.0f, 0.0f, -200.0f},
    };
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    Require(world.AddStaticMesh(vertices, indices, friction), "the ground mesh builds");
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

// The wheels a model defines: bigger at the back, the axles off the bounds' guess, standing on y = 0.
VehicleWheelLayout MakeModelWheelLayout()
{
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.75f, 0.30f, 1.30f), 0.30f, 0.20f};
    layout[1] = {glm::vec3(-0.75f, 0.30f, 1.30f), 0.30f, 0.20f};
    layout[2] = {glm::vec3(0.77f, 0.33f, -1.25f), 0.33f, 0.24f};
    layout[3] = {glm::vec3(-0.77f, 0.33f, -1.25f), 0.33f, 0.24f};
    return layout;
}

void TestFitUsesTheModelsWheels()
{
    const VehicleWheelLayout layout = MakeModelWheelLayout();
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, {}, &layout);
    Require(settings.hasWheelLayout, "the layout is kept");
    Require(std::abs(settings.frontAxleZ - 1.30f) < 1e-4f && std::abs(settings.rearAxleZ + 1.25f) < 1e-4f, "the axles are the wheels'");

    const float rest = ComputeRestSuspensionLength(settings, 9.81f);
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const VehicleWheelGeometry mount = GetVehicleWheelMount(settings, index);
        Require(std::abs(mount.center.y - rest - layout[index].center.y) < 1e-4f, "each wheel hangs the rest length under its mount");
        Require(mount.center.x == layout[index].center.x && mount.center.z == layout[index].center.z, "straight above its centre");
        Require(mount.radius == layout[index].radius && mount.width == layout[index].width, "with its own tyre");
    }
}

void TestCarSitsOnTheModelsWheels()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleWheelLayout layout = MakeModelWheelLayout();
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, {}, &layout);
    const PhysicsPose start{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    const VehicleId car = world.AddVehicle(settings, start);
    Simulate(world, 3.0f);

    Require(world.GetVehicleTelemetry(car).wheelsInContact == 4, "all four wheels touch the ground");
    const PhysicsPose body = world.GetVehiclePose(car);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    for (size_t index = 0; index < kVehicleWheelCount; ++index)
    {
        const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheels[index].pose, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        Require(glm::length(motion.center - layout[index].center) < 0.06f, "wheel " + std::to_string(index) + " rests where the model draws it");
        Require(std::abs(motion.center.y - layout[index].radius) < 0.06f, "and its tyre stands on the ground");
    }
}

// The springs hold a quarter of the weight at the length ComputeRestSuspensionLength gives, whatever the
// car's size, mass and spring rate (the physics engine's frequency mode scaled the spring by the body's
// inertia and sagged 10 to 25% less).
void TestSpringsSettleAtTheRestLength()
{
    struct Case
    {
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        float massKg;
        float frequencyHz;
    };
    const Case cases[] = {
        {kCarMin, kCarMax, 1400.0f, 1.5f},
        {kCarMin, kCarMax, 900.0f, 1.0f},
        {kCarMin, kCarMax, 2500.0f, 3.0f},
        {glm::vec3(-0.75f, 0.0f, -1.6f), glm::vec3(0.75f, 1.2f, 1.6f), 800.0f, 1.5f},
        {glm::vec3(-1.0f, 0.0f, -2.3f), glm::vec3(1.0f, 1.0f, 2.3f), 1100.0f, 2.0f},
    };
    for (const Case& test : cases)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        VehicleSettings tuning;
        tuning.massKg = test.massKg;
        tuning.suspensionFrequencyHz = test.frequencyHz;
        const VehicleSettings settings = FitVehicleSettingsToBounds(test.minBounds, test.maxBounds, tuning);
        const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 4.0f);

        // The solver leaves a few percent of the compression, so the tolerance follows it.
        const float rest = ComputeRestSuspensionLength(settings, 9.81f);
        const float tolerance = 0.002f + 0.06f * (settings.suspensionMaxLength - rest);
        const std::string what = std::to_string(test.massKg) + " kg at " + std::to_string(test.frequencyHz) + " Hz";
        for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
        {
            RequireNear(wheel.suspensionLength, rest, tolerance, "a wheel of " + what + " settles at the rest length");
        }
        RequireNear(world.GetVehiclePose(car).position.y, 0.0f, tolerance, "the body of " + what + " sits at its ride height");
    }
}

// The forces and slip a wheel reports for drawing: the loads carry the car's weight, a car rolling
// straight has no slip to speak of, and steering makes cornering forces that push it round.
void TestWheelStateReportsTyrePhysics()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    VehicleSettings tuning;
    tuning.massKg = 1400.0f;
    tuning.frontTyres = {1.4f, 1.6f, 0.1f, 6.0f, 0.0f, 0.0f};
    tuning.rearTyres = tuning.frontTyres;
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 3.0f);

    float load = 0.0f;
    for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
    {
        Require(wheel.inContact, "every wheel touches the ground");
        load += wheel.suspensionForce;
        RequireNear(wheel.contactNormal.y, 1.0f, 1e-3f, "on flat ground the normal is up");
        RequireNear(wheel.suspensionMinLength, settings.suspensionMinLength, 1e-5f, "the travel's bump end");
        RequireNear(wheel.suspensionMaxLength, settings.suspensionMaxLength, 1e-5f, "and droop end");
        RequireNear(wheel.contactPosition.y, 0.0f, 0.02f, "the contact patch is on the ground");
        RequireNear(glm::length(wheel.mount + wheel.suspensionAxis * wheel.suspensionLength - wheel.pose.position), 0.0f, 0.02f, "the wheel hangs its suspension length below the mount");
        Require(wheel.longitudinalPeakFriction > 1.3f && wheel.longitudinalPeakFriction < 1.5f, "the peak grip is the tyre's, " + std::to_string(wheel.longitudinalPeakFriction));
        Require(wheel.lateralPeakFriction > 1.5f && wheel.lateralPeakFriction < 1.7f, "across too, " + std::to_string(wheel.lateralPeakFriction));
    }
    RequireNear(load, 1400.0f * 9.81f, 1400.0f * 9.81f * 0.05f, "the four loads carry the car's weight");

    // Drive on, then turn right: the tyres push the car towards its right, -X in vehicle space, and
    // the front wheels run at a slip angle.
    VehicleControls controls;
    controls.throttle = 0.6f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 5.0f);
    for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
    {
        Require(std::abs(wheel.slipAngleDegrees) < 1.0f, "straight ahead the slip angle is nothing, " + std::to_string(wheel.slipAngleDegrees));
    }
    controls.steering = 0.5f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 1.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    for (size_t index = 0; index < 2; ++index)
    {
        const VehicleWheelState& wheel = wheels[index];
        Require(std::abs(wheel.slipAngleDegrees) > 1.0f, "a steered wheel slips sideways, " + std::to_string(wheel.slipAngleDegrees));
        Require(wheel.lateralForce * wheel.contactLateral.x < 0.0f && std::abs(wheel.lateralForce) > 500.0f,
                "and pushes the car towards its right, " + std::to_string(wheel.lateralForce) + " N");
        Require(wheel.lateralFriction > 0.0f && wheel.lateralFriction <= wheel.lateralPeakFriction + 1e-3f, "at a friction the curve allows");
    }
}

// A wheel turning more than half a turn per physics step (188 rad/s at 60 Hz: a 0.32 m tyre at 216 km/h,
// or a driven wheel spinning up in the air) has poses that look like it turned the other way, so the roll is
// followed by angle instead. Frame by frame, the wheel the model draws turns as far as the physics engine's.
void TestFastWheelsRollTheRightWay()
{
    PhysicsWorld world;
    VehicleSettings tuning;
    tuning.maxEngineTorque = 900.0f;
    tuning.maxRpm = 9000.0f;
    tuning.gearRatios = {1.0f};
    tuning.finalDriveRatio = 1.0f;
    tuning.tractionControlGrip = 0.0f; // it would slip the clutch on wheels in the air
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    // No ground: the car falls, and its driven wheels spin up as fast as the engine takes them.
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 100000.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);

    constexpr float kFrame = 1.0f / 144.0f;
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const auto rollOf = [&](size_t index)
    {
        const VehicleWheelState wheel = world.GetVehicleWheels(car)[index];
        const VehicleWheelMotion motion = ComputeVehicleWheelMotion(world.GetVehiclePose(car), wheel.pose, identity, glm::vec3(1.0f));
        return std::pair{2.0f * std::atan2(motion.spin.x, motion.spin.w), wheel};
    };
    float fastest = 0.0f;
    for (int frame = 0; frame < 144 * 20 && fastest < 250.0f; ++frame)
    {
        world.Update(kFrame);
        fastest = std::max(fastest, world.GetVehicleWheels(car)[2].angularVelocity);
    }
    Require(fastest > 200.0f, "the driven wheels spin faster than half a turn per step, " + std::to_string(fastest) + " rad/s");

    // Follow the wheel while a frame's turn is still under half a turn, so that the frames themselves can
    // be told apart; that is well past the physics step's half turn.
    float drawn = 0.0f;
    float physical = 0.0f;
    int frames = 0;
    float previousAngle = rollOf(2).first;
    for (; frames < 144; ++frames)
    {
        world.Update(kFrame);
        const auto [angle, wheel] = rollOf(2);
        if (wheel.angularVelocity * kFrame > 3.0f)
        {
            break;
        }
        float turned = angle - previousAngle;
        turned -= 2.0f * 3.14159265f * std::round(turned / (2.0f * 3.14159265f));
        drawn += turned;
        physical += wheel.angularVelocity * kFrame;
        previousAngle = angle;
        RequireNear(std::remainder(angle - wheel.spinAngle, 2.0f * 3.14159265f), 0.0f, 1e-3f, "the pose rolls by the reported roll angle");
    }
    Require(frames >= 10 && physical > 30.0f, "the wheel turned a long way in that time, " + std::to_string(physical) + " rad in " + std::to_string(frames) + " frames");
    RequireNear(drawn, physical, physical * 0.03f, "and the drawn wheel turned as far, forwards");
}

void TestWheelMotionRollsForwardAndSteers()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleWheelLayout layout = MakeModelWheelLayout();
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, {}, &layout);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 2.0f);

    // Between two frames the wheel's top has moved forward, as it does when the car drives forward.
    const auto motionAt = [&](size_t index)
    {
        return ComputeVehicleWheelMotion(world.GetVehiclePose(car), world.GetVehicleWheels(car)[index].pose, identity, glm::vec3(1.0f));
    };
    const VehicleWheelMotion before = motionAt(2);
    world.Update(0.02f);
    const VehicleWheelMotion after = motionAt(2);
    const glm::vec3 topMoved = (after.spin * glm::conjugate(before.spin)) * glm::vec3(0.0f, 1.0f, 0.0f);
    Require(topMoved.z > 0.0f, "a driven wheel rolls its top forward, z = " + std::to_string(topMoved.z));
    Require(glm::length(motionAt(0).steer * glm::vec3(0.0f, 0.0f, 1.0f) - glm::vec3(0.0f, 0.0f, 1.0f)) < 1e-3f, "straight wheels are not steered");

    // Steering right turns the front wheels towards -X and leaves the rear alone.
    controls.steering = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 0.5f);
    Require((motionAt(0).steer * glm::vec3(0.0f, 0.0f, 1.0f)).x < -0.2f, "the front left wheel steers right");
    Require((motionAt(1).steer * glm::vec3(0.0f, 0.0f, 1.0f)).x < -0.2f, "so does the front right");
    Require(std::abs((motionAt(2).steer * glm::vec3(0.0f, 0.0f, 1.0f)).x) < 1e-3f, "the rear does not");
}

void TestWheelMotionSeesTheModelsAxes()
{
    // The same wheel seen by a model that faces the other way: offsets flip in X and Z, and so does
    // the steering's sideways turn.
    const glm::quat halfTurn(0.0f, 0.0f, 1.0f, 0.0f);
    const PhysicsPose body{glm::vec3(5.0f, 1.0f, 2.0f), glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f))};
    const glm::vec3 offset(0.7f, 0.2f, 1.3f);
    const glm::quat steer = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const PhysicsPose wheel{body.position + body.rotation * offset, body.rotation * steer};

    const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheel, halfTurn, glm::vec3(2.0f));
    Require(glm::length(motion.center - glm::vec3(-0.35f, 0.1f, -0.65f)) < 1e-4f, "the offset in the model's axes and unscaled");
    Require(std::abs(glm::degrees(glm::angle(motion.steer)) - 20.0f) < 1e-3f, "the steering angle survives the change of axes");
    Require(glm::length(motion.spin * glm::vec3(0.0f, 1.0f, 0.0f) - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f, "and there is no roll");
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

// An upright two-sided quad in the XY plane, `width` wide and `height` tall, its foot at (x, foot, z):
// a card faces the car whichever way the car comes at it.
void AppendUprightCard(std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices, float x, float z, float width, float height, float foot = 0.0f)
{
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.insert(
        vertices.end(),
        {{x, foot, z}, {x + width, foot, z}, {x + width, foot + height, z}, {x, foot + height, z}});
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    indices.insert(indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
}

// Grass: cards `spacing` apart along X within +-halfWidth, in rows `spacing` apart from z0 to z1.
void AppendCardField(std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices, float halfWidth, float z0, float z1, float spacing, float cardHeight)
{
    for (float z = z0; z < z1; z += spacing)
    {
        for (float x = -halfWidth; x < halfWidth; x += spacing)
        {
            AppendUprightCard(vertices, indices, x, z, spacing * 0.8f, cardHeight);
        }
    }
}

void TestGroundCoverIsRecognised()
{
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;

    AppendCardField(vertices, indices, 3.0f, 8.0f, 11.0f, 0.25f, 0.3f);
    Require(IsGroundCover(vertices, indices), "a field of 30 cm cards is grass");

    vertices.clear();
    indices.clear();
    AppendUprightCard(vertices, indices, 0.0f, 0.0f, 3.0f, 2.4f);
    AppendUprightCard(vertices, indices, 3.0f, 0.0f, 3.0f, 2.4f);
    Require(!IsGroundCover(vertices, indices), "a fence's panels are solid");

    vertices.clear();
    indices.clear();
    AppendUprightCard(vertices, indices, 0.0f, 0.0f, 4.0f, 9.0f);
    Require(!IsGroundCover(vertices, indices), "a tree's card is solid");

    // A drain grating: a flat cut-out with a few upright bars along its rim.
    vertices = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
    indices = {0, 1, 2, 0, 2, 3};
    AppendUprightCard(vertices, indices, 0.0f, 0.0f, 0.2f, 0.1f);
    Require(!IsGroundCover(vertices, indices), "a mostly flat cut-out is a surface to drive on");

    // Grass with a few blades broken off the mesh's indices, and one card that is not a triangle.
    vertices.clear();
    indices.clear();
    AppendCardField(vertices, indices, 1.0f, 0.0f, 1.0f, 0.25f, 0.3f);
    indices.insert(indices.end(), {0, 0, 1, 0, 1, 9999});
    Require(IsGroundCover(vertices, indices), "degenerate and out-of-range triangles are ignored");

    Require(!IsGroundCover({}, {}), "an empty mesh is nothing");
}

// A field of grass cards across the car's path, on a flat ground.
void TestGrassCardsStopACarUnlessTheyAreGroundCover()
{
    std::vector<glm::vec3> cardVertices;
    std::vector<uint32_t> cardIndices;
    AppendCardField(cardVertices, cardIndices, 4.0f, 8.0f, 12.0f, 0.2f, 0.4f);

    const auto driveOver = [&](bool solid)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        if (solid)
        {
            Require(world.AddStaticMesh(cardVertices, cardIndices), "the cards build");
        }
        const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(car, controls);
        Simulate(world, 6.0f);
        return world.GetVehiclePose(car).position.z;
    };

    const float caught = driveOver(true);
    const float through = driveOver(false);
    Require(through > 20.0f, "a car with no cards in its way drives on, z = " + std::to_string(through));
    Require(caught < 12.0f, "solid grass cards stop the car, z = " + std::to_string(caught));
    Require(IsGroundCover(cardVertices, cardIndices), "and that grass is what IsGroundCover leaves out");
}

// A two-sided vertical wall in the plane X = x, from z0 to z1 and `height` tall.
void AddWallMesh(PhysicsWorld& world, float x, float z0, float z1, float height)
{
    const std::vector<glm::vec3> vertices = {{x, 0.0f, z0}, {x, 0.0f, z1}, {x, height, z1}, {x, height, z0}};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    Require(world.AddStaticMesh(vertices, indices), "the wall builds");
}

float RollDegrees(const glm::quat& rotation)
{
    return glm::degrees(std::asin(std::clamp((rotation * glm::vec3(1.0f, 0.0f, 0.0f)).y, -1.0f, 1.0f)));
}

// Friction is the surface's half of a tyre's grip: the same car, full throttle, goes further on
// tarmac than on ice.
void TestSurfaceFrictionSetsGrip()
{
    const auto driveOn = [](float friction)
    {
        PhysicsWorld world;
        AddGroundMesh(world, friction);
        const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(car, controls);
        Simulate(world, 3.0f);
        return world.GetVehiclePose(car).position.z;
    };
    const float tarmac = driveOn(1.0f);
    const float ice = driveOn(0.1f);
    Require(tarmac > ice * 1.5f, "tarmac gives grip ice does not, z = " + std::to_string(tarmac) + " against " + std::to_string(ice));
}

// A car scraping along a wall stops at it and stays on the ground. The wheels' cylinder casts would
// take a wall's face for ground and climb it (without the wall filter this car ends up 0.5 m up, on
// its side); the chassis still hits the wall.
void TestCarScrapingAWallStaysOnTheGround()
{
    PhysicsWorld world;
    // The ground carries on a metre past the wall's foot, as a track's runoff does.
    const std::vector<glm::vec3> groundVertices = {{-200.0f, 0.0f, -200.0f}, {-200.0f, 0.0f, 400.0f}, {4.0f, 0.0f, 400.0f}, {4.0f, 0.0f, -200.0f}};
    Require(world.AddStaticMesh(groundVertices, std::vector<uint32_t>{0, 1, 2, 0, 2, 3}), "the ground builds");
    AddWallMesh(world, 3.0f, -20.0f, 400.0f, 6.0f);
    const glm::quat towardsWall = glm::angleAxis(glm::radians(6.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.75f, 0.33f, 1.3f), 0.33f, 0.30f};
    layout[1] = {glm::vec3(-0.75f, 0.33f, 1.3f), 0.33f, 0.30f};
    layout[2] = {glm::vec3(0.75f, 0.33f, -1.3f), 0.33f, 0.30f};
    layout[3] = {glm::vec3(-0.75f, 0.33f, -1.3f), 0.33f, 0.30f};
    const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, {}, &layout), {glm::vec3(0.0f), towardsWall});
    for (int step = 0; step < 60; ++step)
    {
        world.Update(1.0f / 60.0f);
    }
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);

    float furthest = 0.0f;
    float highest = 0.0f;
    float mostRoll = 0.0f;
    for (int step = 0; step < 600; ++step)
    {
        world.Update(1.0f / 60.0f);
        const PhysicsPose pose = world.GetVehiclePose(car);
        furthest = std::max(furthest, pose.position.x);
        highest = std::max(highest, pose.position.y);
        mostRoll = std::max(mostRoll, std::abs(RollDegrees(pose.rotation)));
    }
    Require(world.GetVehiclePose(car).position.z > 50.0f, "the car drove on along the wall");
    Require(furthest < 3.0f - 0.85f, "the wall stops the chassis, x = " + std::to_string(furthest));
    Require(highest < 0.25f, "and the car stays on the ground, y = " + std::to_string(highest));
    Require(mostRoll < 6.0f, "level, rolling at most " + std::to_string(mostRoll) + " degrees");
}

// A 718 Boxster S PDK as its data.acd gives it (the kn5 import's MINIENGINE_vehicle): 1460 kg, rear
// drive, a turbo four with 386 Nm from 2000 to 4500 rpm, seven gears of a dual-clutch box, semislick
// tyres and a drag area of 0.78 m^2.
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

    VehicleAeroWing body;
    body.name = "BODY";
    body.chord = 1.0f;
    body.span = 1.99f;
    body.position = glm::vec3(0.0f, 0.15f, -0.10f);
    body.dragCurve = {{-2.0f, 0.405f}, {0.0f, 0.39f}, {2.0f, 0.40f}};
    body.liftCurve = {{0.0f, 0.0f}, {2.0f, 0.03f}};
    VehicleAeroWing rear;
    rear.name = "REAR";
    rear.chord = 1.0f;
    rear.span = 1.99f;
    rear.position = glm::vec3(0.0f, 0.34f, -1.6f);
    rear.dragCurve = {{0.0f, 0.005f}, {2.0f, 0.005f}};
    rear.liftCurve = {{0.0f, -0.23f}, {2.0f, -0.21f}};
    spec.aeroWings = {body, rear};
    return spec;
}

// The Nissan GT-R GT3 as Assetto Corsa's ks_nissan_gtr_gt3 data gives it (the kn5 import's
// MINIENGINE_vehicle): 1375 kg, rear drive, double wishbones front and rear with their hardpoints,
// wheel rates, two-stage dampers, bump stops and anti-roll bars.
VehicleCarSpec MakeGtrSpec()
{
    VehicleCarSpec spec;
    spec.massKg = 1375.0f;
    spec.drive = VehicleDrive::RearWheel;
    spec.torqueCurve = {{500.0f, 155.0f}, {1500.0f, 219.0f}, {2500.0f, 299.0f}, {3500.0f, 582.0f}, {4000.0f, 584.0f}, {6000.0f, 578.0f}, {7000.0f, 500.0f}, {8000.0f, 0.0f}};
    spec.minRpm = 2100.0f;
    spec.maxRpm = 7000.0f;
    spec.gearRatios = {2.5f, 2.0f, 1.5217f, 1.2f, 1.0312f, 0.8857f};
    spec.reverseGearRatio = -3.818f;
    spec.finalDriveRatio = 3.4285f;
    spec.gearSwitchSeconds = 0.1f;
    spec.clutchReleaseSeconds = 0.1f;
    spec.engineInertia = 0.135f;
    spec.frontTyres = VehicleTyreSettings{1.5741f, 1.5748f, 0.1058f, 6.04f, 0.85f, 1.8f};
    spec.rearTyres = VehicleTyreSettings{1.6391f, 1.6435f, 0.1058f, 6.04f, 0.85f, 1.8f};
    spec.maxSteerAngleDegrees = 320.0f / 13.6f; // car.ini STEER_LOCK / STEER_RATIO
    spec.brakeTorquePerWheel = 1000.0f;
    spec.frontBrakeShare = 0.67f;
    spec.antiRollBars = true;
    spec.limitedSlipDifferentials = true;

    VehicleSuspensionAxle front;
    front.type = VehicleSuspensionType::DoubleWishbone;
    front.lowerFront = {0.01435f, -0.44595f, -0.1835f};
    front.lowerRear = {-0.34965f, -0.44995f, -0.1835f};
    front.lowerBall = {0.01839f, -0.11121f, -0.11753f};
    front.upperFront = {0.09135f, -0.38995f, 0.3445f};
    front.upperRear = {-0.13765f, -0.39595f, 0.2785f};
    front.upperBall = {-0.05316f, -0.2005f, 0.41825f};
    front.tieInner = {-0.1468f, -0.42095f, -0.15127f};
    front.tieOuter = {-0.1315f, -0.13095f, -0.08929f};
    front.staticCamberDegrees = -2.9f;
    front.toeOutRodLength = -0.0004f;
    front.track = 1.675f;
    front.wheelRate = 153000.0f;
    front.bumpStopRate = 150000.0f;
    front.bumpStopTravel = 0.055f;
    front.reboundStopTravel = 0.05f;
    front.dampBump = 10600.0f;
    front.dampFastBump = 3800.0f;
    front.dampFastBumpThreshold = 0.06f;
    front.dampRebound = 12375.0f;
    front.dampFastRebound = 5485.0f;
    front.dampFastReboundThreshold = 0.12f;
    front.antiRollBarRate = 68000.0f;
    front.hubMass = 59.0f;

    VehicleSuspensionAxle rear;
    rear.type = VehicleSuspensionType::DoubleWishbone;
    rear.lowerFront = {0.32548f, -0.291262f, -0.094691f};
    rear.lowerRear = {0.10478f, -0.480002f, -0.159311f};
    rear.lowerBall = {0.03269f, -0.151896f, -0.093012f};
    rear.upperFront = {0.11759f, -0.425962f, 0.015709f};
    rear.upperRear = {-0.0792f, -0.501202f, 0.027289f};
    rear.upperBall = {0.04021f, -0.165066f, 0.150372f};
    rear.tieInner = {-0.1552f, -0.623202f, -0.128711f};
    rear.tieOuter = {-0.137522f, -0.157345f, 0.00194f};
    rear.staticCamberDegrees = -1.1f;
    rear.toeOutRodLength = 0.0011f;
    rear.track = 1.68f;
    rear.wheelRate = 125000.0f;
    rear.bumpStopRate = 120000.0f;
    rear.bumpStopTravel = 0.06f;
    rear.reboundStopTravel = 0.08f;
    rear.dampBump = 6017.0f;
    rear.dampFastBump = 2875.0f;
    rear.dampFastBumpThreshold = 0.06f;
    rear.dampRebound = 10770.0f;
    rear.dampFastRebound = 4600.0f;
    rear.dampFastReboundThreshold = 0.12f;
    rear.antiRollBarRate = 12050.0f;
    rear.hubMass = 66.0f;
    spec.frontSuspension = front;
    spec.rearSuspension = rear;
    return spec;
}

// The GT-R's wheels where the model draws them: wheelbase 2.78 m with 55.5% on the front, tracks
// 1.675 m and 1.68 m, 0.355 m tyres.
VehicleSettings GtrSettings(bool multibody = true)
{
    VehicleCarSpec spec = MakeGtrSpec();
    if (!multibody)
    {
        spec.frontSuspension.reset();
        spec.rearSuspension.reset();
        spec.suspensionFrequencyHz = 3.5f;
        spec.suspensionDamping = 0.8f;
    }
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
    // A GT3 car's centre of mass, 0.38 m up, over the axles in the data's ratio.
    settings.centerOfMassOffset = glm::vec3(0.0f, 0.38f - settings.chassisCenter.y, -settings.chassisCenter.z);
    return settings;
}

void TestMultibodyCarRestsAtItsDesignPosition()
{
    const VehicleSettings settings = GtrSettings();
    Require(HasSuspensionGeometry(settings), "the GT-R's data carries its linkage");
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 3.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    Require(world.GetVehicleTelemetry(car).wheelsInContact == 4, "all four wheels on the ground");
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const VehicleWheelState& wheel = wheels[index];
        const std::string name = "wheel " + std::to_string(index);
        Require(wheel.multibody, name + " runs on the multibody suspension");
        // The springs' preload puts the car on its design position.
        RequireNear(wheel.travel, 0.0f, 0.006f, name + ": travel at rest");
        RequireNear(wheel.camberDegrees, index < 2 ? -2.9f : -1.1f, 0.2f, name + ": static camber");
        // TOE_OUT: the fronts' rods are set for a little toe-in, the rears' for toe-out.
        Require(std::abs(wheel.toeDegrees) < 1.0f && (index < 2 ? wheel.toeDegrees > 0.0f : wheel.toeDegrees < 0.0f), name + ": toe at rest, " + std::to_string(wheel.toeDegrees));
        // The wheel's drawn pose leans with the camber: its axle points down outboard.
        const glm::vec3 axle = wheel.pose.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        const float outward = index % 2 == 0 ? 1.0f : -1.0f;
        Require(axle.y * outward > 0.01f, name + ": the drawn wheel leans in at the top");
    }
    std::cout << "GT-R at rest: travel FL " << wheels[0].travel * 1000.0f << " mm, RL " << wheels[2].travel * 1000.0f
              << " mm; camber FL " << wheels[0].camberDegrees << " deg, RL " << wheels[2].camberDegrees << " deg\n";
}

void TestMultibodyCarCornersOnItsLinkage()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(GtrSettings(), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    const float speed = world.GetVehicleTelemetry(car).forwardSpeed;
    Require(speed > 15.0f, "the GT-R accelerates on its linkage, " + std::to_string(speed) + " m/s");

    // A steady right-hand turn: the car turns right, rolls left onto its left wheels, and the linkage
    // moves the wheels: the outer (left) front compresses and its camber changes, the fronts steer
    // with Ackermann (the inner wheel turns more).
    const std::vector<VehicleWheelState> straight = world.GetVehicleWheels(car);
    controls.throttle = 0.25f;
    controls.steering = 0.25f;
    world.SetVehicleControls(car, controls);
    float mostRoll = 0.0f;
    for (int frame = 0; frame < 300; ++frame)
    {
        world.Update(1.0f / 144.0f);
        mostRoll = std::max(mostRoll, std::abs(RollDegrees(world.GetVehiclePose(car).rotation)));
    }
    const PhysicsPose pose = world.GetVehiclePose(car);
    const glm::vec3 forward = pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    std::cout << "GT-R turning: roll " << RollDegrees(pose.rotation) << " deg; travel FL " << wheels[0].travel * 1000.0f << " mm, FR "
              << wheels[1].travel * 1000.0f << " mm; toe FL " << wheels[0].toeDegrees << ", FR " << wheels[1].toeDegrees
              << " deg; camber FL " << wheels[0].camberDegrees << ", FR " << wheels[1].camberDegrees << " deg\n";
    Require(forward.x < -0.1f, "steering right turns right, forward.x = " + std::to_string(forward.x));
    Require(mostRoll > 0.2f && mostRoll < 5.0f, "a race car's roll, " + std::to_string(mostRoll) + " deg");
    Require(wheels[0].travel > wheels[1].travel + 0.003f, "the outer (left) front is compressed");
    // Steering right: the left (outer) wheel toes in, the right (inner) toes out by more.
    Require(wheels[0].toeDegrees > 1.0f && wheels[1].toeDegrees < -1.0f, "both fronts steer right");
    // Ackermann: from straight running, the inner wheel turns further than the outer.
    const float outerTurn = wheels[0].toeDegrees - straight[0].toeDegrees;
    const float innerTurn = straight[1].toeDegrees - wheels[1].toeDegrees;
    std::cout << "GT-R steering: outer front turned " << outerTurn << " deg, inner " << innerTurn << " deg\n";
    Require(innerTurn > outerTurn, "Ackermann: the inner wheel turns further");
    Require(std::abs(wheels[2].toeDegrees) < 1.0f && std::abs(wheels[3].toeDegrees) < 1.0f, "the rears only move with their linkage");
}

void TestCarSpecReplacesWhatItKnows()
{
    VehicleSettings tuning;
    tuning.suspensionMinLength = 0.07f;
    tuning.maxHandBrakeTorque = 123.0f;
    const VehicleSettings applied = ApplyCarSpec(tuning, MakeBoxsterSpec());
    Require(applied.massKg == 1460.0f, "the mass");
    Require(applied.drive == VehicleDrive::RearWheel, "the drive");
    Require(std::abs(applied.maxEngineTorque - 386.0f) < 0.01f, "the engine's torque is the curve's peak");
    Require(applied.torqueCurve.size() == 9 && applied.torqueCurve.front().x == 500.0f, "the curve, sorted by rpm");
    Require(applied.minRpm == 900.0f && applied.maxRpm == 7500.0f, "the revs");
    Require(applied.gearRatios.size() == 7 && applied.reverseGearRatio == -3.55f && applied.finalDriveRatio == 3.62f, "the gearbox");
    Require(std::abs(applied.shiftUpRpm - 6600.0f) < 1.0f && std::abs(applied.shiftDownRpm - 2250.0f) < 1.0f, "shifts by the revs");
    Require(applied.gearSwitchSeconds == 0.03f && applied.clutchReleaseSeconds == 0.1f && applied.engineInertia == 0.137f,
            "the clutch and the engine's inertia");
    Require(applied.frontTyres.longitudinalGrip == 1.314f && applied.rearTyres.inertia == 1.97f && applied.rearTyres.postPeakShare == 0.86f,
            "the tyres");
    Require(applied.maxSteerAngleDegrees == 26.7f, "the steering lock");
    Require(applied.maxBrakeTorque == 800.0f && applied.frontBrakeShare == 0.65f && applied.maxHandBrakeTorque == 1000.0f, "the brakes");
    Require(applied.suspensionFrequencyHz == 1.7f && applied.suspensionDamping == 0.7f, "the springs");
    Require(applied.suspensionMinLength == 0.07f, "what the spec does not say stays as tuned");

    // The air: each wing's area times its coefficient at its angle; the body's own damping goes.
    Require(applied.aeroSurfaces.size() == 2 && applied.linearDamping == 0.0f, "the wings replace the body's damping");
    RequireNear(applied.aeroSurfaces[0].dragArea, 0.39f * 1.99f, 1e-4f, "the body's drag area");
    RequireNear(applied.aeroSurfaces[1].downforceArea, 0.23f * 1.99f, 1e-4f, "the rear wing's downforce area");
    Require(applied.aeroSurfaces[1].position.z == -1.6f, "and where it sits");

    // An empty spec changes nothing; nonsense is ignored rather than applied.
    const VehicleSettings untouched = ApplyCarSpec(tuning, VehicleCarSpec{});
    Require(untouched.massKg == tuning.massKg && untouched.maxHandBrakeTorque == 123.0f && untouched.gearRatios.empty() &&
                untouched.aeroSurfaces.empty() && untouched.linearDamping == tuning.linearDamping,
            "an empty spec is a no-op");
    VehicleCarSpec nonsense;
    nonsense.massKg = -5.0f;
    nonsense.maxRpm = 100.0f; // under the default minimum
    nonsense.torqueCurve = {{1000.0f, 0.0f}, {2000.0f, 0.0f}};
    const VehicleSettings ignored = ApplyCarSpec(tuning, nonsense);
    Require(ignored.massKg == tuning.massKg && ignored.maxRpm == tuning.maxRpm && ignored.torqueCurve.empty(), "nonsense is ignored");
}

// The time a car takes to reach a speed from rest on flat tarmac at full throttle, and the highest gear it
// used on the way; `limit` when it does not get there.
float TimeToSpeed(const VehicleSettings& tuning, float metresPerSecond, int& gearReached, float limit = 15.0f)
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    constexpr float kFrame = 1.0f / 144.0f;
    gearReached = 0;
    for (float time = 0.0f; time < limit; time += kFrame)
    {
        world.Update(kFrame);
        const VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
        gearReached = std::max(gearReached, telemetry.gear);
        if (telemetry.forwardSpeed >= metresPerSecond)
        {
            return time;
        }
    }
    return limit;
}

// The car on its own data drives like a quick road car: it launches, changes up through its seven-speed box and
// reaches 100 km/h in a few seconds. The real car does it in 4.2 s; the physics engine's tyres and clutch
// get within a second of that once the engine's real inertia, the dual-clutch's shift times, the tyres' grip and
// air instead of a damped body are in, where the defaults' heavy engine and a 5% damping cost 4 s.
void TestCarOnItsOwnDataAccelerates()
{
    int gear = 0;
    const float own = TimeToSpeed(ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec()), 100.0f / 3.6f, gear);
    std::cout << "0-100 km/h on the Boxster's own data: " << own << " s, gear " << gear << '\n';
    Require(own > 3.5f && own < 6.5f, "0-100 km/h in a fast road car's time, got " + std::to_string(own) + " s");
    Require(gear >= 2, "the gearbox shifts up, reached gear " + std::to_string(gear));

    // What the data changes: the engine's inertia (0.137 kg m^2 against the physics engine's 0.5) is the
    // biggest part of it, the dual clutch's shift times next.
    VehicleCarSpec heavyEngine = MakeBoxsterSpec();
    heavyEngine.engineInertia = 0.5f;
    Require(TimeToSpeed(ApplyCarSpec(VehicleSettings{}, heavyEngine), 100.0f / 3.6f, gear) > own + 0.5f, "a heavy engine costs a launch");
    VehicleCarSpec slowBox = MakeBoxsterSpec();
    slowBox.gearSwitchSeconds = 0.5f;
    slowBox.clutchReleaseSeconds = 0.3f;
    Require(TimeToSpeed(ApplyCarSpec(VehicleSettings{}, slowBox), 100.0f / 3.6f, gear) > own + 0.3f, "and so does a slow gear change");
}

// Grip is what the tyres can hold: the same car on the same tarmac is quicker on grippy tyres than on hard
// ones, and the tyres' friction multiplies the surface's rather than being averaged with it.
void TestTyreGripSetsAcceleration()
{
    int gear = 0;
    const auto timeWith = [&](float grip, float surface)
    {
        VehicleCarSpec spec = MakeBoxsterSpec();
        spec.frontTyres->longitudinalGrip = grip;
        spec.frontTyres->lateralGrip = grip;
        spec.rearTyres->longitudinalGrip = grip;
        spec.rearTyres->lateralGrip = grip;
        PhysicsWorld world;
        AddGroundMesh(world, surface);
        const VehicleId car = world.AddVehicle(
            FitVehicleSettingsToBounds(kCarMin, kCarMax, ApplyCarSpec(VehicleSettings{}, spec)),
            {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(car, controls);
        Simulate(world, 3.0f);
        return world.GetVehiclePose(car).position.z;
    };
    const float grippy = timeWith(1.3f, 1.0f);
    const float hard = timeWith(0.7f, 1.0f);
    Require(grippy > hard * 1.2f, "grippy tyres cover more ground in 3 s, " + std::to_string(grippy) + " m against " + std::to_string(hard) + " m");
    // The surface multiplies: tyres of 1.3 on a surface of 0.5 grip like tyres of 0.65 on tarmac.
    const float wet = timeWith(1.3f, 0.5f);
    const float equivalent = timeWith(0.65f, 1.0f);
    Require(std::abs(wet - equivalent) < 0.12f * equivalent, "the surface's friction multiplies the tyre's, " + std::to_string(wet) + " m against " + std::to_string(equivalent) + " m");
    (void)gear;
}

// The wings slow a car and press it down: after 8 s at full throttle (the ground ends at 200 m) the same car is slower with its
// drag and rides lower on its downforce.
void TestAerodynamicsDragsAndPressesDown()
{
    const auto runFor = [](const VehicleSettings& tuning, float seconds, float& suspensionLength)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(car, controls);
        Simulate(world, seconds);
        suspensionLength = world.GetVehicleWheels(car)[2].suspensionLength;
        return world.GetVehicleTelemetry(car).forwardSpeed;
    };
    VehicleSettings noAir = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    noAir.aeroSurfaces.clear();
    const VehicleSettings air = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    float lengthWithout = 0.0f;
    float lengthWith = 0.0f;
    const float withoutAir = runFor(noAir, 8.0f, lengthWithout);
    const float withAir = runFor(air, 8.0f, lengthWith);
    std::cout << "8 s at full throttle: " << withoutAir * 3.6f << " km/h without air, " << withAir * 3.6f << " with; suspension "
              << lengthWithout << " against " << lengthWith << '\n';
    Require(withoutAir > withAir + 0.5f, "drag holds the car back, " + std::to_string(withoutAir) + " m/s against " + std::to_string(withAir));
    Require(lengthWith < lengthWithout, "downforce compresses the suspension, " + std::to_string(lengthWith) + " against " + std::to_string(lengthWithout));
}

// Runs a car up to 25 m/s, brakes flat out and reports how long the wheels spent locked (slipping past
// 0.5 of the ground speed above 8 m/s, below which a wheel rolls to a stop however gently) and how far the car went.
void BrakeFromSpeed(const VehicleSettings& settings, float& lockedSeconds, float& distance)
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    for (int i = 0; i < 4000 && world.GetVehicleTelemetry(car).forwardSpeed < 25.0f; ++i)
    {
        Simulate(world, 0.01f);
    }
    controls = {};
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);
    const glm::vec3 start = world.GetVehiclePose(car).position;
    lockedSeconds = 0.0f;
    for (float time = 0.0f; time < 10.0f; time += 0.01f)
    {
        const float speed = world.GetVehicleTelemetry(car).forwardSpeed;
        if (speed < 0.3f)
        {
            break;
        }
        for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
        {
            if (speed > 8.0f && 1.0f - std::abs(wheel.angularVelocity) * settings.wheelRadius / speed > 0.5f)
            {
                lockedSeconds += 0.01f / 4.0f;
            }
        }
        Simulate(world, 0.01f);
    }
    distance = glm::length(world.GetVehiclePose(car).position - start);
}

// Brakes hard from 25 m/s: how much of its load each axle turns into braking force (the mean of
// |Fx| / N over the stop, for the two wheels of an axle), and how long the wheels lock, per axle.
struct BrakingReport
{
    float frontTorquePerLoad = 0.0f;
    float rearTorquePerLoad = 0.0f;
    float totalTorque = 0.0f;
    float frontLockedSeconds = 0.0f;
    float rearLockedSeconds = 0.0f;
};

BrakingReport MeasureBraking(const VehicleSettings& settings)
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    for (int i = 0; i < 4000 && world.GetVehicleTelemetry(car).forwardSpeed < 25.0f; ++i)
    {
        Simulate(world, 0.01f);
    }
    controls = {};
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);

    BrakingReport report;
    float frontTorque = 0.0f;
    float frontLoad = 0.0f;
    float rearTorque = 0.0f;
    float rearLoad = 0.0f;
    int samples = 0;
    for (float time = 0.0f; time < 10.0f; time += 0.01f)
    {
        const float speed = world.GetVehicleTelemetry(car).forwardSpeed;
        if (speed < 8.0f)
        {
            break;
        }
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
        for (size_t index = 0; index < wheels.size(); ++index)
        {
            const VehicleWheelState& wheel = wheels[index];
            const bool front = index < 2;
            (front ? frontTorque : rearTorque) += wheel.brakeTorque;
            (front ? frontLoad : rearLoad) += wheel.suspensionForce;
            if (wheel.slipRatio > 0.5f)
            {
                (front ? report.frontLockedSeconds : report.rearLockedSeconds) += 0.01f / 2.0f;
            }
        }
        ++samples;
        Simulate(world, 0.01f);
    }
    report.frontTorquePerLoad = frontLoad > 0.0f ? frontTorque / frontLoad : 0.0f;
    report.rearTorquePerLoad = rearLoad > 0.0f ? rearTorque / rearLoad : 0.0f;
    report.totalTorque = samples > 0 ? (frontTorque + rearTorque) / static_cast<float>(samples) : 0.0f;
    return report;
}

// The brakes' torque goes to the wheels by the load they carry. A fixed 55/45 split gives the rear axle,
// which weight leaves under braking, more torque for its load than the front; sharing by load evens them
// and keeps the total, and at a torque that locks the wheels the rear no longer locks first.
void TestBrakeTorqueFollowsTheLoad()
{
    VehicleSettings fixedSplit = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    fixedSplit.dynamicBrakeBias = false;
    VehicleSettings byLoad = fixedSplit;
    byLoad.dynamicBrakeBias = true;

    fixedSplit.maxBrakeTorque = 600.0f;
    byLoad.maxBrakeTorque = 600.0f;
    const BrakingReport fixedGentle = MeasureBraking(fixedSplit);
    const BrakingReport loadGentle = MeasureBraking(byLoad);
    std::cout << "gentle braking, torque per load front/rear: fixed " << fixedGentle.frontTorquePerLoad << "/" << fixedGentle.rearTorquePerLoad
              << ", by load " << loadGentle.frontTorquePerLoad << "/" << loadGentle.rearTorquePerLoad << "\n";
    Require(fixedGentle.rearTorquePerLoad > fixedGentle.frontTorquePerLoad * 1.04f, "a fixed split gives the unloaded rear more torque for its load");
    RequireNear(loadGentle.rearTorquePerLoad, loadGentle.frontTorquePerLoad, loadGentle.frontTorquePerLoad * 0.03f, "sharing by load evens them");
    RequireNear(fixedGentle.totalTorque, 4.0f * 600.0f, 4.0f * 600.0f * 0.03f, "the fixed split's total");
    RequireNear(loadGentle.totalTorque, 4.0f * 600.0f, 4.0f * 600.0f * 0.03f, "is what sharing by load spreads too");

    fixedSplit.maxBrakeTorque = 1100.0f;
    byLoad.maxBrakeTorque = 1100.0f;
    const BrakingReport fixedHard = MeasureBraking(fixedSplit);
    const BrakingReport loadHard = MeasureBraking(byLoad);
    std::cout << "hard braking, seconds locked front/rear: fixed " << fixedHard.frontLockedSeconds << "/" << fixedHard.rearLockedSeconds
              << ", by load " << loadHard.frontLockedSeconds << "/" << loadHard.rearLockedSeconds << "\n";
    Require(fixedHard.rearLockedSeconds > 0.15f, "a fixed split locks the rear under hard braking, " + std::to_string(fixedHard.rearLockedSeconds));
    Require(loadHard.rearLockedSeconds < 0.05f, "sharing by load keeps it turning, " + std::to_string(loadHard.rearLockedSeconds));
}

void TestAutomaticBrakesDoNotLockTheWheels()
{
    const VehicleSettings car = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    Require(car.maxBrakeTorque == 0.0f, "the brakes are automatic by default");
    const float torque = ComputeBrakeTorquePerWheel(car);
    VehicleSettings heavy = car;
    heavy.massKg = car.massKg * 2.0f;
    RequireNear(ComputeBrakeTorquePerWheel(heavy), torque * 2.0f, 1e-3f, "a car twice as heavy needs brakes twice as strong");
    VehicleSettings grippy = car;
    grippy.frontTyres.longitudinalGrip = 2.2f;
    grippy.rearTyres.longitudinalGrip = 2.2f;
    Require(ComputeBrakeTorquePerWheel(grippy) > torque * 1.8f, "and tyres twice as grippy hold twice the torque");
    VehicleSettings tuned = car;
    tuned.maxBrakeTorque = 321.0f;
    RequireNear(ComputeBrakeTorquePerWheel(tuned), 321.0f, 1e-4f, "a tuned torque stands");

    float lockedSeconds = 0.0f;
    float distance = 0.0f;
    BrakeFromSpeed(car, lockedSeconds, distance);
    std::cout << "automatic brakes: " << torque << " Nm per wheel, 25 m/s to rest in " << distance << " m, wheels locked " << lockedSeconds << " s\n";
    Require(lockedSeconds < 0.1f, "full braking keeps the wheels turning, locked for " + std::to_string(lockedSeconds) + " s");
    Require(distance < 46.0f, "and stops the car in good time, " + std::to_string(distance) + " m");

    VehicleSettings old = car;
    old.dynamicBrakeBias = false;
    old.maxBrakeTorque = 1500.0f;
    old.frontBrakeShare = 0.5f;
    float oldLocked = 0.0f;
    float oldDistance = 0.0f;
    BrakeFromSpeed(old, oldLocked, oldDistance);
    Require(oldLocked > 0.5f, "the old 1500 Nm at an even split does lock them (" + std::to_string(oldLocked) + " s)");
}

// Full throttle from a standstill for six seconds: how far the rear tyres turn faster than the ground passes
// under them (their rim speed over the car's, less one, the mean of the two wheels, at over 2 m/s), its mean
// and peak, and how much the two wheels differ.
struct LaunchReport
{
    float meanSlip = 0.0f;
    float peakSlip = 0.0f;
    float meanGap = 0.0f;
};

LaunchReport MeasureLaunch(const VehicleSettings& tuning)
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning), {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    LaunchReport report;
    int samples = 0;
    for (int frame = 0; frame < 6 * 60; ++frame)
    {
        world.Update(1.0f / 60.0f);
        const float speed = world.GetVehicleTelemetry(car).forwardSpeed;
        if (speed < 2.0f)
        {
            continue;
        }
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
        const float left = wheels[2].angularVelocity * wheels[2].radius / speed - 1.0f;
        const float right = wheels[3].angularVelocity * wheels[3].radius / speed - 1.0f;
        report.meanSlip += 0.5f * (left + right);
        report.peakSlip = std::max(report.peakSlip, 0.5f * (left + right));
        report.meanGap += std::abs(left - right);
        ++samples;
    }
    Require(samples > 100, "the car gets going");
    report.meanSlip /= static_cast<float>(samples);
    report.meanGap /= static_cast<float>(samples);
    return report;
}

// At full throttle in first and second gear the engine asks for more than the tyres can hold, and the wheels
// spin up on its revs, several times the speed of the ground. Traction control slips the clutch past what
// the tyres take, and a limited-slip differential keeps the two wheels of an axle turning together where the
// physics engine's own let them take turns spinning.
void TestDrivenWheelsKeepNearTheGround()
{
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    tuning.tractionControlGrip = 0.0f;
    const LaunchReport free = MeasureLaunch(tuning);
    tuning.tractionControlGrip = 0.85f;
    const LaunchReport held = MeasureLaunch(tuning);
    std::cout << "launch, rear tyres' speed over the ground's: free " << free.meanSlip << " mean, " << free.peakSlip << " peak; traction control " << held.meanSlip
              << " mean, " << held.peakSlip << " peak, the two wheels " << held.meanGap << " apart\n";
    Require(free.meanSlip > 0.15f && free.peakSlip > 0.5f, "without traction control the tyres spin up, " + std::to_string(free.meanSlip));
    Require(held.meanSlip < 0.25f && held.peakSlip < free.peakSlip * 0.85f, "with it they stay near the ground's speed, " + std::to_string(held.meanSlip) + ", " + std::to_string(held.peakSlip));

    tuning.limitedSlipDifferentials = false;
    tuning.tractionControlGrip = 0.0f; // with it holding both tyres to the ground, an open differential has nothing to show
    const LaunchReport open = MeasureLaunch(tuning);
    Require(held.meanGap < 0.03f, "the limited slip keeps the two rear wheels together, " + std::to_string(held.meanGap));
    // At 1000 Hz a level car launches almost symmetrically, so the open differential is judged against the limited slip.
    Require(open.meanGap > held.meanGap * 5.0f, "an open differential does not, " + std::to_string(open.meanGap));
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
        TestGroundCoverIsRecognised();
        TestGrassCardsStopACarUnlessTheyAreGroundCover();
        TestSurfaceFrictionSetsGrip();
        TestCarScrapingAWallStaysOnTheGround();
        TestDegenerateMeshIsRejected();
        TestAutomaticBrakesDoNotLockTheWheels();
        TestBrakeTorqueFollowsTheLoad();
        TestCarSpecReplacesWhatItKnows();
        TestCarOnItsOwnDataAccelerates();
        TestTyreGripSetsAcceleration();
        TestAerodynamicsDragsAndPressesDown();
        TestCarSettlesOnTheGround();
        TestCarDrivesSteersAndReverses();
        TestCarRotatedAtStartDrivesItsOwnWay();
        TestFitUsesTheModelsWheels();
        TestCarSitsOnTheModelsWheels();
        TestSpringsSettleAtTheRestLength();
        TestWheelStateReportsTyrePhysics();
        TestFastWheelsRollTheRightWay();
        TestDrivenWheelsKeepNearTheGround();
        TestMultibodyCarRestsAtItsDesignPosition();
        TestMultibodyCarCornersOnItsLinkage();
        TestWheelMotionRollsForwardAndSteers();
        TestWheelMotionSeesTheModelsAxes();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "vehicle physics tests failed: " << exception.what() << '\n';
        return 1;
    }

    std::cout << "vehicle physics tests passed\n";
    return 0;
}
