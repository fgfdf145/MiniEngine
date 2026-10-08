#include "ae86_car_spec.h"
#include "gtr_car_spec.h"

#include <engine/physics/collision_filter.h>
#include <engine/physics/physics_world.h>
#include <engine/physics/water_surface.h>
#include <engine/physics/vehicle_settings.h>
#include <engine/physics/vehicle_suspension.h>
#include <engine/physics/vehicle_wheel_motion.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;
using me::test::MakeAe86Spec;
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

// The GT-R's gearbox as the vehicle builds it: idle 2100 rpm, limiter 7000, six gears.
VehicleGearbox GtrGearbox()
{
    VehicleSettings settings = ApplyCarSpec(VehicleSettings{}, MakeGtrSpec());
    VehicleGearbox gearbox;
    gearbox.forwardRatios = settings.gearRatios;
    gearbox.reverseRatio = std::abs(settings.reverseGearRatio);
    gearbox.shiftPoints = ComputeVehicleShiftPoints(settings);
    gearbox.idleRpm = settings.minRpm;
    gearbox.switchSeconds = 0.1f;
    gearbox.releaseSeconds = 0.1f;
    gearbox.latencySeconds = 0.5f;
    return gearbox;
}

// Runs the gearbox at a steady speed and throttle until it has settled.
void SettleGearbox(const VehicleGearbox& gearbox, VehicleGearboxState& state, float forward, float outputRpm)
{
    for (int step = 0; step < 5000; ++step)
    {
        UpdateAutomaticGearbox(gearbox, state, forward, outputRpm, 1.0f / 1000.0f);
    }
}

// The shift points sit inside the engine's revs: the GT-R idles at 2100 rpm, and the old fixed 30% of the
// limiter put its change down there, where the engine's revs, held at the idle, never went below it.
void TestShiftPointsFollowTheRevRange()
{
    const VehicleShiftPoints points = ComputeVehicleShiftPoints(ApplyCarSpec(VehicleSettings{}, MakeGtrSpec()));
    Require(points.downClosed > 2100.0f + 100.0f, "off the throttle it changes down above the idle, " + std::to_string(points.downClosed));
    Require(points.upLight > points.downClosed && points.upFull > points.upLight && points.upFull < 7000.0f, "a light foot changes up early, a full one under the limiter");
    Require(points.downFull > points.downClosed && points.downFull < points.upFull, "a full throttle kicks down sooner");
}

void TestGearboxPicksTheGearBySpeedAndThrottle()
{
    const VehicleGearbox gearbox = GtrGearbox();
    const auto rpmInGear = [&](int gear, float outputRpm)
    {
        return VehicleGearRpm(gearbox, gear, outputRpm);
    };

    // Accelerating hard it holds each gear to the full-throttle point, then changes up one.
    VehicleGearboxState state;
    const float output = gearbox.shiftPoints.upFull / gearbox.forwardRatios[0] * 1.02f;
    SettleGearbox(gearbox, state, 1.0f, output);
    Require(state.gear == 2 && state.clutch == 1.0f, "full throttle past the point changes up to second, in " + std::to_string(state.gear));

    // The same speed on a light throttle cruises in a higher gear, under the light change-up point.
    VehicleGearboxState light;
    SettleGearbox(gearbox, light, 0.2f, output);
    Require(light.gear > 2, "a light foot changes up early, in " + std::to_string(light.gear));
    Require(rpmInGear(light.gear, output) < gearbox.shiftPoints.upFull, "and keeps the revs down");

    // Flooring it from a slower cruise kicks down, as far as the lower gear still has room before its change up.
    VehicleGearboxState cruise;
    const float slower = 2400.0f;
    SettleGearbox(gearbox, cruise, 0.2f, slower);
    const int cruising = cruise.gear;
    SettleGearbox(gearbox, cruise, 1.0f, slower);
    Require(cruise.gear < cruising, "flooring it kicks down from " + std::to_string(cruising));
    Require(rpmInGear(cruise.gear, slower) < gearbox.shiftPoints.upFull, "into a gear below the limiter");

    // Braking to a stop it changes down all the way: in top gear at walking pace the engine would turn below its idle.
    VehicleGearboxState braking;
    braking.gear = 6;
    SettleGearbox(gearbox, braking, 0.0f, 100.0f);
    Require(braking.gear == 1, "stopping changes down to first, in " + std::to_string(braking.gear));
    Require(braking.clutch == 0.0f, "with the clutch open so the idling engine does not push the car");
    SettleGearbox(gearbox, braking, 0.0f, 0.0f);
    UpdateAutomaticGearbox(gearbox, braking, 1.0f, 0.0f, 1.0f / 1000.0f);
    Require(braking.gear == 1 && braking.clutch > 0.0f, "the throttle pulls away in first");

    // Reverse throttle selects reverse; forward again selects first.
    UpdateAutomaticGearbox(gearbox, braking, -0.5f, 0.0f, 1.0f / 1000.0f);
    Require(braking.gear == -1, "reverse");
    UpdateAutomaticGearbox(gearbox, braking, 0.5f, 0.0f, 1.0f / 1000.0f);
    Require(braking.gear == 1, "and back to first");
}

// The game's change times each way (CHANGE_UP_TIME, CHANGE_DN_TIME) hold the clutch open for as long, and
// an upshift cuts the engine for AUTO_CUTOFF_TIME, here longer than the change.
void TestGearboxChangeTimesAndUpshiftCut()
{
    VehicleGearbox gearbox = GtrGearbox();
    gearbox.switchSeconds = 0.24f;
    gearbox.switchDownSeconds = 0.30f;
    gearbox.upshiftCutSeconds = 0.35f;
    constexpr float kStep = 1.0f / 1000.0f;
    const auto openSeconds = [&](VehicleGearboxState& state, float outputRpm, int expectedGear, float& cut)
    {
        UpdateAutomaticGearbox(gearbox, state, 1.0f, outputRpm, kStep);
        Require(state.gear == expectedGear, "the change to " + std::to_string(expectedGear) + ", in " + std::to_string(state.gear));
        cut = state.cutLeft;
        int steps = 1;
        while (state.clutch == 0.0f && steps < 2000)
        {
            UpdateAutomaticGearbox(gearbox, state, 1.0f, outputRpm, kStep);
            ++steps;
        }
        return steps * kStep;
    };
    VehicleGearboxState up;
    float cut = 0.0f;
    const float upSeconds = openSeconds(up, gearbox.shiftPoints.upFull / gearbox.forwardRatios[0] * 1.02f, 2, cut);
    RequireNear(upSeconds, 0.24f, 0.003f, "a change up takes CHANGE_UP_TIME");
    RequireNear(cut, 0.35f - kStep, 0.002f, "and cuts the engine for AUTO_CUTOFF_TIME");

    VehicleGearboxState down;
    down.gear = 3;
    const float downSeconds = openSeconds(down, gearbox.shiftPoints.downFull / gearbox.forwardRatios[2] * 0.9f, 2, cut);
    RequireNear(downSeconds, 0.30f, 0.003f, "a change down takes CHANGE_DN_TIME");
    Require(cut == 0.0f, "without a cut");
}

// Any steady speed and throttle settles in one gear: the box does not change back and forth.
void TestGearboxDoesNotHunt()
{
    const VehicleGearbox gearbox = GtrGearbox();
    for (float throttle = 0.0f; throttle <= 1.0f; throttle += 0.1f)
    {
        for (float output = 200.0f; output < 3000.0f; output += 50.0f)
        {
            VehicleGearboxState state;
            SettleGearbox(gearbox, state, throttle, output);
            const int settled = state.gear;
            int changes = 0;
            for (int step = 0; step < 5000; ++step)
            {
                const int before = state.gear;
                UpdateAutomaticGearbox(gearbox, state, throttle, output, 1.0f / 1000.0f);
                changes += state.gear != before ? 1 : 0;
            }
            Require(changes == 0 && state.gear == settled, "the gear holds at throttle " + std::to_string(throttle) + ", output " + std::to_string(output) + " rpm");
        }
    }
}

// The manual box changes one gear per press, through neutral to reverse, and refuses what would hurt
// the engine: a change down onto the limiter, reverse while the car still rolls forward.
void TestManualGearboxChangesWhenAsked()
{
    VehicleGearbox gearbox = GtrGearbox();
    gearbox.limiterRpm = gearbox.shiftPoints.upFull; // first gear at the test speed revs past it
    constexpr float kStep = 1.0f / 1000.0f;
    const auto run = [&](VehicleGearboxState& state, int shifts, float forward, float outputRpm, float engineRpm, bool declutch = false)
    {
        UpdateManualGearbox(gearbox, state, shifts, forward, outputRpm, kStep, engineRpm, declutch);
        for (int step = 0; step < 1000; ++step)
        {
            UpdateManualGearbox(gearbox, state, 0, forward, outputRpm, kStep, engineRpm, declutch);
        }
    };

    // It holds first past the automatic's change-up point: only the driver changes up.
    VehicleGearboxState state;
    const float fast = gearbox.shiftPoints.upFull / gearbox.forwardRatios[0] * 1.02f;
    run(state, 0, 1.0f, fast, VehicleGearRpm(gearbox, 1, fast));
    Require(state.gear == 1 && state.clutch == 1.0f, "the manual box holds first, in " + std::to_string(state.gear));
    UpdateManualGearbox(gearbox, state, 1, 1.0f, fast, kStep, VehicleGearRpm(gearbox, 1, fast));
    Require(state.gear == 2 && state.clutch == 0.0f && state.revMatch, "a press changes up with the clutch open");
    run(state, 0, 1.0f, fast, VehicleGearRpm(gearbox, 2, fast));
    Require(state.clutch == 1.0f, "and the clutch bites again");

    // Changing down two at that speed would put first past the limiter: it stops at second.
    run(state, 1, 1.0f, fast, VehicleGearRpm(gearbox, 2, fast));
    Require(state.gear == 3, "third");
    run(state, -2, 1.0f, fast, VehicleGearRpm(gearbox, 3, fast));
    Require(state.gear == 2, "a change down onto the limiter is refused, in " + std::to_string(state.gear));

    // Slower, down through first to neutral, which opens the clutch; reverse waits for the car to stop.
    run(state, -2, 0.0f, 300.0f, 1000.0f);
    Require(state.gear == 0 && state.clutch == 0.0f, "neutral, in " + std::to_string(state.gear));
    run(state, -1, 0.0f, fast, 1000.0f);
    Require(state.gear == 0, "no reverse while rolling, in " + std::to_string(state.gear));
    run(state, -1, 0.0f, 0.0f, 1000.0f);
    Require(state.gear == -1, "reverse once stopped");
    run(state, -1, 0.0f, 0.0f, 1000.0f);
    Require(state.gear == -1, "and nothing below it");
    run(state, 2, 0.0f, 0.0f, 1000.0f);
    Require(state.gear == 1, "up through neutral to first");
    run(state, 0, 0.5f, 0.0f, 3000.0f);
    Require(state.clutch > 0.0f, "the throttle moves off in first");

    // The clutch pedal (or the hand brake) declutches; let go, it bites again.
    run(state, 0, 1.0f, 1000.0f, 3000.0f, true);
    Require(state.gear == 1 && state.clutch == 0.0f, "the pedal opens the clutch, in gear");
    run(state, 0, 1.0f, 1000.0f, 3000.0f);
    Require(state.clutch == 1.0f, "and it bites when let go");
}

// The automatic takes the driver's changes as a tiptronic's paddles: it holds the gear until
// manualHoldSeconds pass without another, changing itself only on the limiter or where the engine would
// labour, and picks the gear again after that.
void TestAutomaticGearboxHoldsThePaddlesGear()
{
    VehicleGearbox gearbox = GtrGearbox();
    gearbox.limiterRpm = gearbox.shiftPoints.upFull * 1.05f;
    constexpr float kStep = 1.0f / 1000.0f;
    // The engine turns at the gear's speed (the clutch shut), unless `engineRpm` says otherwise.
    const auto run = [&](VehicleGearboxState& state, int shifts, float forward, float outputRpm, float seconds, float engineRpm = 0.0f)
    {
        UpdateAutomaticGearbox(gearbox, state, forward, outputRpm, kStep,
                               engineRpm > 0.0f ? engineRpm : VehicleGearRpm(gearbox, state.gear, outputRpm), shifts);
        for (float time = kStep; time < seconds; time += kStep)
        {
            UpdateAutomaticGearbox(gearbox, state, forward, outputRpm, kStep,
                                   engineRpm > 0.0f ? engineRpm : VehicleGearRpm(gearbox, state.gear, outputRpm));
        }
    };

    // Cruising on a light throttle the box sits in a high gear; a press down holds the lower one.
    VehicleGearboxState state;
    const float cruise = gearbox.shiftPoints.upLight / gearbox.forwardRatios[3] * 1.1f;
    run(state, 0, 0.2f, cruise, 5.0f);
    const int automatic = state.gear;
    Require(automatic >= 4 && state.manualHoldLeft == 0.0f, "the automatic cruises in a high gear, " + std::to_string(automatic));
    run(state, -1, 0.2f, cruise, 0.001f);
    Require(state.gear == automatic - 1 && state.manualHoldLeft > 0.0f, "a press changes down and holds the gear");
    run(state, 0, 0.2f, cruise, gearbox.manualHoldSeconds - 1.0f);
    Require(state.gear == automatic - 1, "the gear holds while the hold lasts, in " + std::to_string(state.gear));
    run(state, 0, 0.2f, cruise, 2.0f);
    Require(state.gear == automatic && state.manualHoldLeft == 0.0f, "then the automatic picks again, " + std::to_string(state.gear));

    // Held in a gear, full throttle runs on past the automatic's change-up point, up only on the limiter.
    const float pulling = gearbox.shiftPoints.upFull / gearbox.forwardRatios[1] * 1.02f;
    VehicleGearboxState second;
    second.gear = 2;
    run(second, 1, 1.0f, gearbox.shiftPoints.upLight / gearbox.forwardRatios[1], 0.001f);
    Require(second.gear == 3 && second.manualHoldLeft > 0.0f, "a press up from second");
    run(second, -1, 1.0f, pulling, 1.0f);
    Require(second.gear == 2, "and back down to second while second stays under the limiter");
    run(second, 0, 1.0f, pulling, 1.0f);
    Require(second.gear == 2, "full throttle holds second past the change-up point");
    run(second, 0, 1.0f, gearbox.limiterRpm / gearbox.forwardRatios[1], 1.0f);
    Require(second.gear == 3, "and changes up on the limiter, in " + std::to_string(second.gear));

    // A change down that would rev past the limiter is refused; a gear the engine would labour in
    // changes down by itself.
    VehicleGearboxState fast;
    fast.gear = 3;
    const float third = gearbox.limiterRpm / gearbox.forwardRatios[1] * 1.05f; // second would pass the limiter
    run(fast, -1, 1.0f, third, 0.001f);
    Require(fast.gear == 3 && fast.manualHoldLeft > 0.0f, "no change down onto the limiter, in " + std::to_string(fast.gear));
    const float labouring = gearbox.shiftPoints.downClosed / gearbox.forwardRatios[3] * 0.9f;
    VehicleGearboxState slow;
    slow.gear = 3;
    run(slow, 1, 0.3f, labouring, 0.001f);
    Require(slow.gear == 4, "a press up into fourth");
    run(slow, 0, 0.3f, labouring, 2.0f);
    Require(slow.gear < 4 && slow.manualHoldLeft > 0.0f, "fourth labouring changes down, held, in " + std::to_string(slow.gear));
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

void TestWaterSurfaceHeights()
{
    WaterSurface water(10.0f);
    // A sloping square from x = 0 to 20 (z 0 to 20), 1 m high at x = 0 and 3 m at x = 20, and a pond at
    // 5 m over part of it.
    const std::vector<glm::vec3> sea = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 20.0f}, {20.0f, 3.0f, 20.0f}, {20.0f, 3.0f, 0.0f}};
    const std::vector<uint32_t> square = {0, 1, 2, 0, 2, 3};
    water.Add(sea, square);
    Require(water.TriangleCount() == 2, "two water triangles");
    RequireNear(water.HeightAt(10.0f, 5.0f).value_or(-1.0f), 2.0f, 1e-4f, "height inside the square");
    RequireNear(water.HeightAt(10.0f, 10.0f).value_or(-1.0f), 2.0f, 1e-4f, "height on the shared diagonal");
    Require(!water.HeightAt(25.0f, 5.0f).has_value(), "no water past the edge");
    const std::vector<glm::vec3> pond = {{2.0f, 5.0f, 2.0f}, {2.0f, 5.0f, 6.0f}, {6.0f, 5.0f, 6.0f}, {6.0f, 5.0f, 2.0f}};
    water.Add(pond, square);
    RequireNear(water.HeightAt(4.0f, 4.0f).value_or(-1.0f), 5.0f, 1e-4f, "the higher surface counts where two overlap");
    const std::vector<glm::vec3> wall = {{0.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f}, {0.0f, 5.0f, 5.0f}};
    const std::vector<uint32_t> one = {0, 1, 2};
    water.Add(wall, one);
    Require(water.TriangleCount() == 4, "a vertical triangle has no top and is left out");
}

// A car that rolls into deep water floats while it fills, then sinks to the bottom, and its engine
// drowns on the way down.
void TestCarFloatsThenSinksInWater()
{
    PhysicsWorld world;
    const std::vector<glm::vec3> bottom = {{-200.0f, -12.0f, -200.0f}, {-200.0f, -12.0f, 200.0f}, {200.0f, -12.0f, 200.0f}, {200.0f, -12.0f, -200.0f}};
    const std::vector<uint32_t> square = {0, 1, 2, 0, 2, 3};
    Require(world.AddStaticMesh(bottom, square), "the sea bed builds");
    const std::vector<glm::vec3> surface = {{-200.0f, 0.0f, -200.0f}, {-200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, -200.0f}};
    world.AddWaterSurface(surface, square);
    Require(world.GetWaterTriangleCount() == 2, "the water's two triangles");

    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});

    Simulate(world, 2.0f);
    VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    PhysicsPose pose = world.GetVehiclePose(car);
    Require(pose.position.y > -2.0f, "the car floats at first, y = " + std::to_string(pose.position.y));
    Require(telemetry.submergedShare > 0.1f && telemetry.submergedShare < 0.95f,
            "partly under the surface while it floats, share " + std::to_string(telemetry.submergedShare));

    Simulate(world, 40.0f);
    telemetry = world.GetVehicleTelemetry(car);
    pose = world.GetVehiclePose(car);
    Require(pose.position.y < -10.0f, "it has sunk to the bottom, y = " + std::to_string(pose.position.y));
    Require(telemetry.flooded > 0.9f, "full of water, " + std::to_string(telemetry.flooded));
    Require(telemetry.engineDrowned, "and the engine has drowned");

    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    const glm::dvec3 before = world.GetVehiclePose(car).position;
    Simulate(world, 3.0f);
    const glm::dvec3 after = world.GetVehiclePose(car).position;
    Require(glm::length(glm::vec2(after.x - before.x, after.z - before.z)) < 1.0f, "a drowned engine drives nowhere");

    world.ResetVehicle(car, {glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Require(!world.GetVehicleTelemetry(car).engineDrowned, "a reset car is dry again");
}

// A tunnel under the water (III's Porter Tunnel): a car driving down into it from dry land is not in the
// sea above it, and neither floats nor drowns. One sinking from the surface stays in the water however
// deep it goes (TestCarFloatsThenSinksInWater).
void TestCarInATunnelUnderTheWaterStaysDry()
{
    PhysicsWorld world;
    // A road at y = -20 from z = -200 to 200; the water covers it 20 m above, but for a dry band from
    // z = -40 to -20 where the car starts, whichever way it drives.
    const std::vector<glm::vec3> road = {{-20.0f, -20.0f, -200.0f}, {-20.0f, -20.0f, 200.0f}, {20.0f, -20.0f, 200.0f}, {20.0f, -20.0f, -200.0f}};
    const std::vector<uint32_t> square = {0, 1, 2, 0, 2, 3};
    Require(world.AddStaticMesh(road, square), "the tunnel's road builds");
    for (const auto& [z0, z1] : {std::pair{-200.0f, -40.0f}, std::pair{-20.0f, 200.0f}})
    {
        const std::vector<glm::vec3> surface = {{-200.0f, 0.0f, z0}, {-200.0f, 0.0f, z1}, {200.0f, 0.0f, z1}, {200.0f, 0.0f, z0}};
        world.AddWaterSurface(surface, square);
    }

    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, -19.5f, -30.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.throttle = 0.6f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 8.0f);
    const PhysicsPose pose = world.GetVehiclePose(car);
    const VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    Require(std::abs(pose.position.z + 30.0f) > 25.0f, "the car has driven in under the water, z = " + std::to_string(pose.position.z));
    Require(pose.position.y < -18.0f, "on the tunnel's road, y = " + std::to_string(pose.position.y));
    Require(telemetry.submergedShare == 0.0f && telemetry.flooded == 0.0f && !telemetry.engineDrowned, "and dry");
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

// A car on its roof is found the ground under (through itself), and set down upright there drives on.
void TestCarRecoversFromItsRoof()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const glm::quat upsideDown = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(5.0f, 2.0f, 3.0f), upsideDown});
    Simulate(world, 2.0f);
    PhysicsPose pose = world.GetVehiclePose(car);
    Require((pose.rotation * glm::vec3(0.0f, 1.0f, 0.0f)).y < -0.5f, "the car lies on its roof");

    const std::optional<double> ground = world.FindGroundBelow(pose.position + glm::dvec3(0.0, 1.0, 0.0), 100.0);
    Require(ground.has_value() && std::abs(*ground) < 1e-3f, "the ray finds the ground through the car, not the car");
    Require(!world.FindGroundBelow(glm::dvec3(500.0, 1.0, 0.0), 100.0).has_value(), "and nothing off the edge of the world");

    world.ResetVehicle(car, {glm::vec3(pose.position.x, *ground + 0.15f, pose.position.z), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 2.0f);
    pose = world.GetVehiclePose(car);
    Require(world.GetVehicleTelemetry(car).wheelsInContact == 4, "set upright, it stands on its four wheels");
    Require((pose.rotation * glm::vec3(0.0f, 1.0f, 0.0f)).y > 0.99f, "level");

    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    const float startZ = pose.position.z;
    Simulate(world, 2.0f);
    Require(world.GetVehiclePose(car).position.z > startZ + 3.0f, "and drives away");
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

void TestCarDrivesOnTheManualGearbox()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    const PhysicsPose start{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    const VehicleId car = world.AddVehicle(settings, start);
    Simulate(world, 1.0f);

    VehicleControls controls;
    controls.manualGearbox = true;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 4.0f);
    VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.gear == 1, "the manual box stays in first, in " + std::to_string(telemetry.gear));
    Require(telemetry.forwardSpeed > 3.0f, "and drives, speed " + std::to_string(telemetry.forwardSpeed));

    controls.gearShifts = 1;
    world.SetVehicleControls(car, controls);
    controls.gearShifts = 0;
    world.SetVehicleControls(car, controls); // a press is counted once, however often the controls come
    Simulate(world, 1.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.gear == 2, "a change up makes second, in " + std::to_string(telemetry.gear));

    // Pulling back brakes to a stop and does not reverse.
    controls.throttle = -1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 5.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(std::abs(telemetry.forwardSpeed) < 0.3f, "pulling back stops the car, speed " + std::to_string(telemetry.forwardSpeed));
    Require(telemetry.gear == 2, "in its gear");

    // Down to reverse, and the throttle backs up.
    controls.throttle = 0.0f;
    controls.gearShifts = -3;
    world.SetVehicleControls(car, controls);
    controls.gearShifts = 0;
    Simulate(world, 0.1f);
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.gear == -1, "three changes down from second make reverse, in " + std::to_string(telemetry.gear));
    Require(telemetry.forwardSpeed < -1.0f, "the throttle backs up, speed " + std::to_string(telemetry.forwardSpeed));

    // The clutch held: still in reverse, the throttle revs the engine and no longer drives.
    controls.throttle = 0.0f;
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 5.0f);
    controls.brake = 0.0f;
    controls.throttle = 1.0f;
    controls.clutchPedal = true;
    world.SetVehicleControls(car, controls);
    Simulate(world, 1.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.gear == -1, "the clutch keeps the gear, in " + std::to_string(telemetry.gear));
    Require(telemetry.engineRpm > settings.minRpm + 500.0f, "the engine revs free, " + std::to_string(telemetry.engineRpm));
    Require(std::abs(telemetry.forwardSpeed) < 0.5f, "and the car stays, speed " + std::to_string(telemetry.forwardSpeed));

    // Let go, it bites and the car backs away.
    controls.clutchPedal = false;
    world.SetVehicleControls(car, controls);
    Simulate(world, 2.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.forwardSpeed < -1.0f, "letting the clutch go drives, speed " + std::to_string(telemetry.forwardSpeed));
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
    SetAxleTyres(tuning, true, {1.4f, 1.6f, 0.1f, 6.0f, 0.0f, 0.0f});
    SetAxleTyres(tuning, false, tuning.tyres[0]);
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
        RequireNear(static_cast<float>(glm::length(wheel.mount + glm::dvec3(wheel.suspensionAxis * wheel.suspensionLength) - wheel.pose.position)), 0.0f, 0.02f, "the wheel hangs its suspension length below the mount");
        Require(wheel.longitudinalPeakFriction > 1.3f && wheel.longitudinalPeakFriction < 1.5f, "the peak grip is the tyre's, " + std::to_string(wheel.longitudinalPeakFriction));
        Require(wheel.lateralPeakFriction > 1.5f && wheel.lateralPeakFriction < 1.7f, "across too, " + std::to_string(wheel.lateralPeakFriction));
    }
    RequireNear(load, 1400.0f * 9.81f, 1400.0f * 9.81f * 0.05f, "the four loads carry the car's weight");

    // Drive on, then turn right: the tyres push the car towards its right, -X in vehicle space, and
    // the front wheels run at a slip angle. Gently: this tall, softly sprung car on 1.6 g tyres lifts
    // its inside wheels near the limit.
    VehicleControls controls;
    controls.throttle = 0.6f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 5.0f);
    for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
    {
        Require(std::abs(wheel.slipAngleDegrees) < 1.0f, "straight ahead the slip angle is nothing, " + std::to_string(wheel.slipAngleDegrees));
    }
    controls.steering = 0.1f;
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

// Engine braking comes from the car's data (Assetto Corsa's COAST_REF). Revved up in the air in its one
// gear and the throttle shut, the car's driven wheels slow only by what the engine takes back through
// the clutch: an engine dragging 300 Nm at 5000 rpm takes far more than the physics engine's own small
// drag on its revs. The wheels themselves no longer lose spin of their own.
float WheelSpinLostOnTheCoast(float coastTorque)
{
    PhysicsWorld world;
    VehicleSettings tuning;
    tuning.gearRatios = {1.0f};
    tuning.finalDriveRatio = 1.0f;
    tuning.tractionControlGrip = 0.0f;
    tuning.engineCoastTorque = coastTorque;
    tuning.engineCoastRpm = 5000.0f;
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 100000.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 1.0f);
    const float revved = world.GetVehicleWheels(car)[2].angularVelocity;
    const float undriven = world.GetVehicleWheels(car)[0].angularVelocity;
    world.SetVehicleControls(car, VehicleControls{});
    Simulate(world, 2.0f);
    RequireNear(world.GetVehicleWheels(car)[0].angularVelocity, undriven, 1e-3f, "an undriven wheel in the air keeps its spin");
    return revved - world.GetVehicleWheels(car)[2].angularVelocity;
}

void TestEngineBrakingFromTheData()
{
    const float plain = WheelSpinLostOnTheCoast(0.0f);
    const float coasting = WheelSpinLostOnTheCoast(300.0f);
    std::cout << "throttle shut for 2 s in the air: the driven wheels lose " << plain << " rad/s on the physics engine's drag, " << coasting
              << " with 300 Nm of engine braking\n";
    Require(coasting > 2.0f * plain && coasting > 20.0f, "the data's engine braking slows the wheels");
}

// A turbo's boost as the game has it: (throttle * rpm / reference)^gamma of the maximum, held to the
// wastegate. In the car it follows that level with the game's lag, a share kept each of the game's 333 Hz
// steps: LAG_UP 0.9988 builds it with a time constant of 2.5 s, LAG_DN 0.995 lets it go in 0.6 s.
void TestTurboSpoolsWithItsLag()
{
    const VehicleTurbo r34{1.2f, 0.4f, 3400.0f, 2.0f, 0.9988f, 0.995f};
    RequireNear(VehicleTurboBoost(r34, 5000.0f, 1.0f), 0.4f, 1e-6f, "full throttle: the wastegate");
    RequireNear(VehicleTurboBoost(r34, 3000.0f, 0.5f), 1.2f * std::pow(0.5f * 3000.0f / 3400.0f, 2.0f), 1e-5f, "half throttle scales the revs before gamma");
    RequireNear(VehicleTurboBoost(r34, 5000.0f, 0.0f), 0.0f, 1e-6f, "throttle shut: none");

    VehicleSettings tuning;
    tuning.gearRatios = {1.0f};
    tuning.finalDriveRatio = 1.0f;
    tuning.tractionControlGrip = 0.0f;
    tuning.maxEngineTorque = 300.0f;
    tuning.torqueCurve = {{1000.0f, 300.0f}, {7000.0f, 300.0f}};
    tuning.turbos = {VehicleTurbo{1.0f, 0.5f, 1000.0f, 1.0f, 0.9988f, 0.995f}};
    RequireNear(VehicleTurboTorqueScale(tuning, 4000.0f, 0.0f), 1.0f / 1.5f, 1e-5f, "no boost yet: the curve's torque without its boost");
    RequireNear(VehicleTurboTorqueScale(tuning, 4000.0f, 0.5f), 1.0f, 1e-5f, "full boost: the curve's");
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    PhysicsWorld world;
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 100000.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 2.5f);
    const float built = world.GetVehicleTelemetry(car).turboBoost;
    world.SetVehicleControls(car, VehicleControls{});
    Simulate(world, 0.6f);
    const float fallen = world.GetVehicleTelemetry(car).turboBoost;
    std::cout << "turbo: " << built << " of 0.5 after 2.5 s at full throttle, " << fallen << " left 0.6 s after lifting\n";
    RequireNear(built, 0.5f * (1.0f - std::exp(-1.0f)), 0.03f, "builds with a 2.5 s time constant");
    RequireNear(fallen, built * std::exp(-1.0f), 0.02f, "and falls with a 0.6 s one");
}

// The car's data for its differential's lock on the overrun and preload, and its tyres' load sensitivity
// (the starting compound's LS_EXPX and LS_EXPY, their mean, or the one given).
void TestCarDataGivesDifferentialAndTyreSensitivity()
{
    VehicleCarSpec spec;
    spec.limitedSlipDifferentials = true;
    spec.differentialPower = 0.5f;
    spec.differentialCoast = 0.3f;
    spec.differentialPreload = 10.0f;
    VehicleTyreCompound compound;
    compound.front.values = {{"LS_EXPX", 0.9f}, {"LS_EXPY", 0.84f}, {"RIM_RADIUS", 0.254f}, {"PRESSURE_STATIC", 28.0f}, {"RELAXATION_LENGTH", 0.0757f}, {"CX_MULT", 1.04f}};
    compound.rear.values = {{"LS_EXPY", 0.8f}};
    spec.tyreCompounds = {compound};
    spec.defaultTyreCompound = 0;
    const VehicleSettings settings = ApplyCarSpec(VehicleSettings{}, spec);
    RequireNear(settings.limitedSlipLock, 0.5f, 1e-6f, "the lock under power");
    RequireNear(settings.limitedSlipCoast, 0.3f, 1e-6f, "on the overrun");
    RequireNear(settings.limitedSlipPreload, 10.0f, 1e-6f, "and the preload");
    RequireNear(settings.tyres[0].longitudinalLoadExponent, 0.9f, 1e-6f, "the front tyres' load sensitivity along the wheel");
    RequireNear(settings.tyres[0].lateralLoadExponent, 0.84f, 1e-6f, "and across it");
    RequireNear(settings.tyres[2].longitudinalLoadExponent, 0.8f, 1e-6f, "the rear's, the one given for both");
    RequireNear(settings.tyres[2].lateralLoadExponent, 0.8f, 1e-6f, "the rear's across");
    // The brush tyre's build from the same compound: the pressure from psi.
    RequireNear(settings.tyres[0].rimRadius, 0.254f, 1e-6f, "the rim's radius");
    RequireNear(settings.tyres[0].inflationPressure, 28.0f * 6894.757f, 1.0f, "the inflation pressure in pascals");
    RequireNear(settings.tyres[0].relaxationLength, 0.0757f, 1e-6f, "the relaxation length");
    RequireNear(settings.tyres[0].longitudinalStiffnessRatio, 1.04f, 1e-6f, "the tread's fore-aft stiffness ratio");
    Require(settings.tyres[2].relaxationLength == 0.0f && settings.tyres[2].inflationPressure == 0.0f, "none given: the brush tyre's defaults");
    Require(ApplyCarSpec(VehicleSettings{}, VehicleCarSpec{}).limitedSlipPreload < 0.0f, "no data: the default preload");

    // The gearbox's and clutch's figures, and the electronics: the game's traction control replaces ours.
    spec.changeDownSeconds = 0.3f;
    spec.autoCutoffSeconds = 0.24f;
    spec.clutchMaxTorque = 750.0f;
    spec.electronics["TRACTION_CONTROL"] = {{"PRESENT", 1.0f}, {"ACTIVE", 1.0f}, {"SLIP_RATIO_LIMIT", 0.12f}, {"MIN_SPEED_KMH", 40.0f}, {"RATE_HZ", 250.0f}};
    const VehicleSettings withElectronics = ApplyCarSpec(VehicleSettings{}, spec);
    Require(withElectronics.gearSwitchDownSeconds == 0.3f && withElectronics.upshiftCutSeconds == 0.24f && withElectronics.clutchMaxTorque == 750.0f,
            "change down time, upshift cut and clutch limit");
    Require(withElectronics.tractionControlGrip == 0.0f && withElectronics.tcSlipRatioLimit == 0.12f && withElectronics.tcMinSpeedKmh == 40.0f &&
                withElectronics.tcRateHz == 250.0f,
            "the game's traction control instead of ours");
    spec.electronics["TRACTION_CONTROL"]["ACTIVE"] = 0.0f;
    const VehicleSettings tcOff = ApplyCarSpec(VehicleSettings{}, spec);
    Require(tcOff.tractionControlGrip == 0.0f && tcOff.tcSlipRatioLimit == 0.0f, "a car whose traction control is off has none");
}

// The game's traction control on a slippery road (grip 0.5): rolled up gently to 8 m/s, then the
// throttle floored. Cutting it while a driven wheel spins past its limit holds the spin down; without
// it the wheels spin up.
float WorstSpinOnASlipperyRoad(float tcLimit)
{
    VehicleSettings tuning;
    tuning.tractionControlGrip = 0.0f;
    tuning.tcSlipRatioLimit = tcLimit;
    tuning.tcRateHz = 250.0f;
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    PhysicsWorld world;
    AddGroundMesh(world, 0.5f);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, -150.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 0.15f;
    world.SetVehicleControls(car, controls);
    for (int frame = 0; frame < 144 * 30 && world.GetVehicleTelemetry(car).forwardSpeed < 8.0f; ++frame)
    {
        world.Update(1.0f / 144.0f);
    }
    Require(world.GetVehicleTelemetry(car).forwardSpeed >= 8.0f, "the car rolls up to speed");
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    float worst = 0.0f;
    for (int frame = 0; frame < 144 * 2; ++frame)
    {
        world.Update(1.0f / 144.0f);
        worst = std::max(worst, world.GetVehicleTelemetry(car).spinSlip);
    }
    return worst;
}

void TestGameTractionControlCutsTheThrottle()
{
    const float without = WorstSpinOnASlipperyRoad(0.0f);
    const float with = WorstSpinOnASlipperyRoad(0.12f);
    std::cout << "flooring it at 8 m/s on grip 0.5: worst wheelspin " << without << " without traction control, " << with << " with the game's\n";
    Require(without > 0.5f, "without it the wheels spin up");
    Require(with < 0.3f && with < 0.5f * without, "with it the spin is held down");
}

// Braked to a stop on brush tyres, the car settles: it does not rock back and forth on its tyres'
// carcasses once it has stopped (it did for over two seconds at 7 Hz with the carcass barely damped
// fore and aft).
void TestCarSettlesAfterBrakingToAStop()
{
    VehicleSettings tuning;
    tuning.tyreModel = VehicleTyreModel::Brush;
    const VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning);
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, -150.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    for (int frame = 0; frame < 144 * 20 && world.GetVehicleTelemetry(car).forwardSpeed < 15.0f; ++frame)
    {
        world.Update(1.0f / 144.0f);
    }
    controls = {};
    controls.brake = 0.6f;
    world.SetVehicleControls(car, controls);
    for (int frame = 0; frame < 144 * 10 && world.GetVehicleTelemetry(car).forwardSpeed > 0.01f; ++frame)
    {
        world.Update(1.0f / 144.0f);
    }
    // Half a second for the body to rock back on its springs, then the car must stay put.
    Simulate(world, 0.5f);
    float most = 0.0f;
    for (int frame = 0; frame < 144 * 1.5f; ++frame)
    {
        world.Update(1.0f / 144.0f);
        most = std::max(most, std::abs(world.GetVehicleTelemetry(car).forwardSpeed));
    }
    std::cout << "braked to a stop: from 0.5 s after it the car moves at most " << most * 1000.0f << " mm/s\n";
    Require(most < 0.02f, "it stays put once it has settled, " + std::to_string(most) + " m/s");
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
    const PhysicsPose wheel{body.position + glm::dvec3(body.rotation * offset), body.rotation * steer};

    const VehicleWheelMotion motion = ComputeVehicleWheelMotion(body, wheel, halfTurn, glm::vec3(2.0f));
    Require(glm::length(motion.center - glm::vec3(-0.35f, 0.1f, -0.65f)) < 1e-4f, "the offset in the model's axes and unscaled");
    Require(std::abs(glm::degrees(glm::angle(motion.steer)) - 20.0f) < 1e-3f, "the steering angle survives the change of axes");
    Require(glm::length(motion.spin * glm::vec3(0.0f, 1.0f, 0.0f) - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f, "and there is no roll");
}

void TestUpdateRunsFixedSteps()
{
    PhysicsWorld world;
    Require(world.Update(PhysicsWorld::kDefaultStepSeconds * 0.5f) == 0, "half a step runs none");
    Require(world.Update(PhysicsWorld::kDefaultStepSeconds * 0.6f) == 1, "the remainder carries over");
    Require(world.Update(10.0f) == world.MaxStepsPerUpdate(), "a long frame is capped");
    Require(world.Update(0.0f) == 1, "the capped backlog keeps at most one step");
    Require(world.Update(0.0f) == 0, "nothing is left over");
}

void TestStepIsSettable()
{
    PhysicsWorld world;
    Require(world.GetStepSeconds() == PhysicsWorld::kDefaultStepSeconds, "a new world steps at the default");
    world.SetStepSeconds(1.0f / 500.0f);
    Require(world.GetStepSeconds() == 1.0f / 500.0f, "the step is taken as given");
    Require(world.Update(4.5e-3f) == 2, "and runs at its rate");
    Require(world.MaxStepsPerUpdate() == 25, "the catch-up is a time, " + std::to_string(world.MaxStepsPerUpdate()) + " steps at 500 Hz");
    Require(world.Update(10.0f) == 25, "a long frame is capped at it");
    world.SetStepSeconds(1.0f);
    Require(world.GetStepSeconds() == PhysicsWorld::kMaxStepSeconds, "a step too long is clamped");
    world.SetStepSeconds(0.0f);
    Require(world.GetStepSeconds() == PhysicsWorld::kMinStepSeconds, "and one too short");
}

// The same drive at other step rates (both tyre models): the car stays stable and gets about as far.
void TestCarDrivesAtAnyStepRate()
{
    for (const VehicleTyreModel tyreModel : {VehicleTyreModel::PhysicsEngine, VehicleTyreModel::Brush})
    {
        const auto drive = [tyreModel](float rateHz)
        {
            PhysicsWorld world;
            world.SetStepSeconds(1.0f / rateHz);
            AddGroundMesh(world);
            VehicleSettings settings = FitVehicleSettingsToBounds(kCarMin, kCarMax);
            settings.tyreModel = tyreModel;
            const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
            Simulate(world, 1.0f);
            VehicleControls controls;
            controls.throttle = 1.0f;
            world.SetVehicleControls(car, controls);
            Simulate(world, 4.0f);
            const PhysicsPose pose = world.GetVehiclePose(car);
            const glm::vec3 up = pose.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
            const std::string name = std::string(tyreModel == VehicleTyreModel::Brush ? "brush" : "slip curves") + " at " +
                                     std::to_string(static_cast<int>(rateHz)) + " Hz";
            Require(std::isfinite(pose.position.z) && up.y > 0.99f, name + ": the car stays upright, up.y = " + std::to_string(up.y));
            Require(std::abs(pose.position.x) < 1.0f, name + ": and straight, x = " + std::to_string(pose.position.x));
            return world.GetVehicleTelemetry(car).forwardSpeed;
        };
        const float reference = drive(1000.0f);
        for (const float rate : {333.0f, 2000.0f})
        {
            const float speed = drive(rate);
            Require(std::abs(speed - reference) < 0.05f * reference,
                    "at " + std::to_string(static_cast<int>(rate)) + " Hz the car reaches " + std::to_string(speed) + " m/s, at 1000 Hz " +
                        std::to_string(reference));
        }
    }
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
        furthest = std::max(furthest, static_cast<float>(pose.position.x));
        highest = std::max(highest, static_cast<float>(pose.position.y));
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

// The GT-R's wheels where the model draws them: wheelbase 2.78 m with 55.5% on the front, tracks
// 1.675 m and 1.68 m, 0.355 m tyres. Without `unsprung` the data loses its tyre rates, and the
// multibody suspension runs with massless wheels.
VehicleSettings GtrSettings(bool multibody = true, bool unsprung = true)
{
    VehicleCarSpec spec = MakeGtrSpec();
    if (!unsprung)
    {
        spec.frontSuspension->tyreRate = 0.0f;
        spec.rearSuspension->tyreRate = 0.0f;
    }
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

void TestMultibodyCarRestsAtItsDesignPosition(bool unsprung)
{
    const VehicleSettings settings = GtrSettings(true, unsprung);
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
        Require(wheel.unsprungMass == unsprung, name + (unsprung ? " has its hub mass" : " has massless wheels"));
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
    // The linkage for drawing sits at the wheels: every joint on an upright within half a metre of one.
    const VehicleLinkage linkage = world.GetVehicleLinkage(car);
    Require(linkage.links.size() >= 16 && linkage.carriers.size() >= 12 && !linkage.joints.empty(), "the linkage to draw");
    for (const glm::dvec3& joint : linkage.joints)
    {
        float nearest = 1e9f;
        for (const VehicleWheelState& wheel : wheels)
        {
            nearest = std::min(nearest, static_cast<float>(glm::length(joint - wheel.pose.position)));
        }
        Require(nearest < 0.5f, "a joint at its wheel, " + std::to_string(nearest) + " m away");
    }
    std::cout << (unsprung ? "GT-R at rest (hub masses): travel FL " : "GT-R at rest (massless wheels): travel FL ") << wheels[0].travel * 1000.0f << " mm, RL " << wheels[2].travel * 1000.0f
              << " mm; camber FL " << wheels[0].camberDegrees << " deg, RL " << wheels[2].camberDegrees << " deg\n";
}

void TestMultibodyCarCornersOnItsLinkage(bool unsprung)
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(GtrSettings(true, unsprung), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
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
    const float turnSpeed = world.GetVehicleTelemetry(car).forwardSpeed;
    float lateralLoad = 0.0f;
    for (const VehicleWheelState& wheel : wheels)
    {
        lateralLoad += wheel.lateralForce;
    }
    std::cout << (unsprung ? "GT-R turning (hub masses): " : "GT-R turning (massless wheels): ") << turnSpeed << " m/s, "
              << lateralLoad / (GtrSettings().massKg * 9.81f) << " g; loads FL " << wheels[0].suspensionForce << " FR " << wheels[1].suspensionForce
              << " RL " << wheels[2].suspensionForce << " RR " << wheels[3].suspensionForce << "; roll " << RollDegrees(pose.rotation) << " deg; travel FL " << wheels[0].travel * 1000.0f << " mm, FR "
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

// With hub masses the tyre is a spring of its own: at rest each one is squashed by its load over its
// rate, the loads add up to the car's weight (hubs included), and the hub is drawn that much nearer
// the ground than the tyre's radius.
void TestUnsprungCarStandsOnItsTyres()
{
    const VehicleSettings settings = GtrSettings();
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 3.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    float total = 0.0f;
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const VehicleWheelState& wheel = wheels[index];
        const std::string name = "wheel " + std::to_string(index);
        Require(wheel.inContact && wheel.unsprungMass, name + " stands on its tyre");
        total += wheel.suspensionForce;
        RequireNear(wheel.tyreDeflection, wheel.suspensionForce / 284861.0f, 0.0005f, name + ": the tyre squashed by its load");
        // The hub over the ground: the tyre's radius less its deflection (the camber's lean aside).
        RequireNear(wheel.pose.position.y, 0.355f - wheel.tyreDeflection, 0.012f, name + ": the hub's height over the ground");
    }
    const float weight = settings.massKg * 9.81f;
    RequireNear(total, weight, 0.01f * weight, "the tyres carry the whole car");
    std::cout << "GT-R on its tyres: load FL " << wheels[0].suspensionForce << " N, deflection " << wheels[0].tyreDeflection * 1000.0f
              << " mm, hub " << wheels[0].pose.position.y * 1000.0f << " mm up; RL " << wheels[2].suspensionForce << " N, "
              << wheels[2].tyreDeflection * 1000.0f << " mm\n";
}

// Dropped from half a metre the hubs hang out with the tyres unloaded, then the car lands
// on them and comes back to rest at the design position.
void TestUnsprungWheelsHangInTheAirAndLand()
{
    const VehicleSettings settings = GtrSettings();
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 0.15f);
    std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const std::string name = "wheel " + std::to_string(index);
        Require(!wheels[index].inContact && wheels[index].tyreDeflection == 0.0f, name + " is off the ground");
        // Falling, the hub weighs nothing against the body: the spring pushes it out until it goes
        // slack (about its static deflection, 20 mm), short of the droop stop.
        const float droop = index < 2 ? 0.05f : 0.08f;
        Require(wheels[index].travel < -0.015f && wheels[index].travel > -droop - 1e-4f,
                name + " hangs out on its slack spring, " + std::to_string(wheels[index].travel));
    }
    float mostLoad = 0.0f;
    float mostBump = 0.0f;
    constexpr float kFrame = 1.0f / 144.0f;
    for (float time = 0.0f; time < 3.0f; time += kFrame)
    {
        world.Update(kFrame);
        for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
        {
            Require(std::isfinite(wheel.travel) && std::isfinite(wheel.suspensionForce), "the landing stays finite");
            mostLoad = std::max(mostLoad, wheel.suspensionForce);
            mostBump = std::max(mostBump, wheel.travel);
        }
    }
    wheels = world.GetVehicleWheels(car);
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const std::string name = "wheel " + std::to_string(index);
        Require(wheels[index].inContact, name + " is back on the ground");
        RequireNear(wheels[index].travel, 0.0f, 0.006f, name + " is back at the design position");
    }
    const float quarter = settings.massKg * 9.81f * 0.25f;
    std::cout << "GT-R dropped 0.5 m: most tyre load " << mostLoad / quarter << " x a quarter of the weight, most bump " << mostBump * 1000.0f << " mm\n";
    Require(mostLoad > 2.0f * quarter, "the landing loads the tyres");
}

// Dropped a metre, the GT-R lands on its hubs' end stops and its rims: stiff springs that cushion the
// blow over some milliseconds. The rigid stops they replace (the hub's travel held at its end, the
// physics engine's hard point past the tyre) took the body's speed within a step: some 150 g.
void TestHardLandingIsCushioned()
{
    const VehicleSettings settings = GtrSettings();
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    constexpr float kStep = PhysicsWorld::kDefaultStepSeconds;
    float lastY = world.GetVehiclePose(car).position.y;
    float lastRate = 0.0f;
    float hardest = 0.0f;
    float mostBump = 0.0f;
    float mostRise = 0.0f;
    for (float time = 0.0f; time < 3.0f; time += kStep)
    {
        world.Update(kStep);
        const float y = world.GetVehiclePose(car).position.y;
        const float rate = (y - lastY) / kStep;
        hardest = std::max(hardest, (rate - lastRate) / kStep / 9.81f);
        mostRise = std::max(mostRise, rate);
        lastY = y;
        lastRate = rate;
        for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
        {
            Require(std::isfinite(wheel.travel) && std::isfinite(wheel.suspensionForce), "the landing stays finite");
            mostBump = std::max(mostBump, wheel.travel);
        }
    }
    std::cout << "GT-R dropped 1 m: hardest " << hardest << " g, most bump " << mostBump * 1000.0f << " mm, rose again at up to " << mostRise << " m/s\n";
    Require(hardest < 40.0f, "the end stops cushion the landing");
    Require(mostBump < 0.2f, "the hubs stay near their end stops");
    Require(mostRise < 3.0f, "the car is not thrown back up");
    for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
    {
        Require(wheel.inContact && std::abs(wheel.travel) < 0.006f, "the car settles on its wheels");
    }
}

// A 2 cm bump across the road at speed: the hubs ride over it (the front ones' travel and tyre loads
// jump), the body hardly feels it, and the car goes on straight.
void TestUnsprungCarTakesABump()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    world.AddStaticBox(glm::vec3(0.0f, 0.01f, 45.0f), glm::vec3(5.0f, 0.01f, 0.4f));
    const VehicleId car = world.AddVehicle(GtrSettings(), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    constexpr float kFrame = 1.0f / 144.0f;
    float time = 0.0f;
    while (world.GetVehiclePose(car).position.z < 30.0f && time < 10.0f)
    {
        world.Update(kFrame);
        time += kFrame;
    }
    controls.throttle = 0.3f;
    world.SetVehicleControls(car, controls);
    const float speed = world.GetVehicleTelemetry(car).forwardSpeed;
    const float rest = world.GetVehicleWheels(car)[0].suspensionForce;
    float mostTravel = 0.0f;
    float mostLoad = 0.0f;
    float lowestBody = 1.0f;
    float highestBody = -1.0f;
    const float bodyBefore = world.GetVehiclePose(car).position.y;
    while (world.GetVehiclePose(car).position.z < 60.0f && time < 20.0f)
    {
        world.Update(kFrame);
        time += kFrame;
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
        mostTravel = std::max(mostTravel, wheels[0].travel);
        mostLoad = std::max(mostLoad, wheels[0].suspensionForce);
        const float body = world.GetVehiclePose(car).position.y - bodyBefore;
        lowestBody = std::min(lowestBody, body);
        highestBody = std::max(highestBody, body);
    }
    Simulate(world, 1.5f);
    const PhysicsPose pose = world.GetVehiclePose(car);
    const glm::vec3 forward = pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    std::cout << "GT-R over a 2 cm bump at " << speed << " m/s: front travel up to " << mostTravel * 1000.0f << " mm, tyre load up to "
              << mostLoad / rest << " x rest; body moved " << lowestBody * 1000.0f << " to " << highestBody * 1000.0f << " mm\n";
    Require(speed > 15.0f, "the GT-R reaches the bump at speed");
    Require(mostTravel > 0.008f, "the front hub rides up over the bump");
    Require(mostLoad > 1.3f * rest, "the bump loads the front tyre");
    Require(highestBody - lowestBody < 0.02f, "the body hardly feels it");
    Require(std::abs(forward.x) < 0.05f && forward.z > 0.99f, "the car goes on straight");
    for (const VehicleWheelState& wheel : world.GetVehicleWheels(car))
    {
        Require(wheel.inContact && std::abs(wheel.travel) < 0.01f, "the car runs on with its wheels settled");
    }
}

// Triangles facing up, whichever way round they were listed.
void AddUpwardMesh(PhysicsWorld& world, const std::vector<glm::vec3>& vertices, std::vector<uint32_t> indices)
{
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        const glm::vec3 normal = glm::cross(vertices[indices[index + 1]] - vertices[indices[index]], vertices[indices[index + 2]] - vertices[indices[index]]);
        if (normal.y < 0.0f)
        {
            std::swap(indices[index + 1], indices[index + 2]);
        }
    }
    Require(world.AddStaticMesh(vertices, indices), "the mesh builds");
}

// The GT-R with an Assetto Corsa body as the R34 has one (colliders.ini, GROUND_ENABLE): a floor box and a
// splitter box 6 cm off the road, 0.8 m ahead of the front axle.
VehicleSettings GtrWithSplitter()
{
    VehicleSettings settings = GtrSettings();
    const float centreOfMassY = (settings.chassisCenter + settings.centerOfMassOffset).y;
    const float centreOfMassZ = (settings.chassisCenter + settings.centerOfMassOffset).z;
    settings.carColliders = {
        {glm::vec3(0.0f, 0.16f + 0.075f - centreOfMassY, -centreOfMassZ), glm::vec3(1.75f, 0.15f, 3.4f), true},
        {glm::vec3(0.0f, 0.06f + 0.075f - centreOfMassY, settings.frontAxleZ + 0.8f - centreOfMassZ), glm::vec3(1.57f, 0.15f, 0.35f), true},
    };
    return settings;
}

// Drives a car straight up +Z from z = -60 at `kmh` across whatever is around z = 0 and reports the worst
// one-frame loss of speed (km/h) and the fastest the body rose (m/s).
struct Crossing
{
    float startKmh = 0.0f;
    float worstFrameLossKmh = 0.0f;
    float fastestRise = 0.0f;
    float endKmh = 0.0f;
};

Crossing CrossAtSpeed(PhysicsWorld& world, VehicleId car, float kmh)
{
    world.ResetVehicle(car, {glm::vec3(0.0f, 0.0f, -60.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    constexpr float kFrame = 1.0f / 60.0f;
    VehicleControls controls;
    Crossing crossing;
    float lastKmh = 0.0f;
    float lastY = world.GetVehiclePose(car).position.y;
    for (float time = 0.0f; time < 30.0f && world.GetVehiclePose(car).position.z < 12.0f; time += kFrame)
    {
        const float speed = world.GetVehicleTelemetry(car).forwardSpeed * 3.6f;
        controls.throttle = speed < kmh ? 1.0f : 0.0f;
        world.SetVehicleControls(car, controls);
        world.Update(kFrame);
        const PhysicsPose pose = world.GetVehiclePose(car);
        const float now = world.GetVehicleTelemetry(car).forwardSpeed * 3.6f;
        const float rise = (pose.position.y - lastY) / kFrame;
        lastY = pose.position.y;
        if (pose.position.z > -8.0f)
        {
            if (crossing.startKmh == 0.0f)
            {
                crossing.startKmh = now;
            }
            else
            {
                crossing.worstFrameLossKmh = std::max(crossing.worstFrameLossKmh, lastKmh - now);
                crossing.fastestRise = std::max(crossing.fastestRise, rise);
            }
        }
        lastKmh = now;
    }
    crossing.endKmh = lastKmh;
    return crossing;
}

// San Andreas's roads have vertices standing out of them: a tent 21 cm high, rising over 5 m and falling
// over 20. A splitter corner passing over its peak caught the edges round it, which pushed the body
// back and up: 7 km/h gone in a frame and the front wheels thrown off the road. The splitter now scrapes
// over it.
void TestSplitterRidesOverARoadSpike()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    // Los Santos's triangles round (807.46, 12.59, 1323.2), from the road's height and the lane's middle:
    // the peak sits on a ridge across the lane, 5 m up from one side and 20 m down to the other.
    const std::vector<glm::vec3> tent = {
        {0.8f, 0.21f, 0.0f},   {-7.19f, 0.0f, 0.0f}, {9.96f, 0.0f, 0.0f}, {14.96f, 0.0f, -5.0f},
        {-12.19f, 0.0f, -5.0f}, {-7.19f, 0.0f, 20.0f}, {1.83f, 0.0f, 20.0f}, {9.96f, 0.0f, 20.0f},
    };
    AddUpwardMesh(world, tent, {4, 3, 1, 2, 0, 3, 2, 7, 0, 0, 1, 3, 0, 5, 1, 5, 0, 6, 6, 0, 7});
    const VehicleId car = world.AddVehicle(GtrWithSplitter(), {glm::vec3(0.0f, 0.0f, -60.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    const Crossing crossing = CrossAtSpeed(world, car, 60.0f);
    std::cout << "GT-R with a splitter over a 21 cm road spike at " << crossing.startKmh << " km/h: worst frame lost " << crossing.worstFrameLossKmh
              << " km/h, body rose at up to " << crossing.fastestRise << " m/s\n";
    Require(crossing.startKmh > 55.0f, "the car reaches the spike at speed");
    Require(crossing.worstFrameLossKmh < 2.0f, "the splitter does not catch on the spike");
    Require(crossing.fastestRise < 1.5f, "the spike does not throw the car");
}

// A kerb top with no riser (the map's pavements), 15 cm up: more than the wheels' bump travel and tyre
// take, so the wheel meets the physics engine's hard stop on the kerb's edge. Along the edge's normal,
// leaning back, that stop threw the car up at several metres a second; now the car is lifted onto it.
void TestWheelMountsATallKerbWithoutLeaping()
{
    PhysicsWorld world;
    AddUpwardMesh(world, {{-50.0f, 0.0f, -300.0f}, {50.0f, 0.0f, -300.0f}, {50.0f, 0.0f, 0.0f}, {-50.0f, 0.0f, 0.0f}}, {0, 1, 2, 0, 2, 3});
    AddUpwardMesh(world, {{-50.0f, 0.15f, 0.0f}, {50.0f, 0.15f, 0.0f}, {50.0f, 0.15f, 100.0f}, {-50.0f, 0.15f, 100.0f}}, {0, 1, 2, 0, 2, 3});
    const VehicleId car = world.AddVehicle(GtrSettings(), {glm::vec3(0.0f, 0.0f, -60.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    const Crossing crossing = CrossAtSpeed(world, car, 60.0f);
    std::cout << "GT-R onto a 15 cm kerb with no riser at " << crossing.startKmh << " km/h: worst frame lost " << crossing.worstFrameLossKmh
              << " km/h, body rose at up to " << crossing.fastestRise << " m/s, " << crossing.endKmh << " km/h after\n";
    Require(crossing.startKmh > 55.0f, "the car reaches the kerb at speed");
    Require(crossing.fastestRise < 3.0f, "the kerb does not throw the car");
    Require(crossing.endKmh > 45.0f, "the car drives on over the kerb");
}

// BeamNG's impact bumps (Grid Map v2, the game's own triangles): blocks 2.1 m across, 4 cm high, their
// sides rising over 2.8 cm (55 degrees) to an 11 cm top, laid on the road without sharing its vertices,
// one under each side of the car in turn (the left 2.93 m after the right, every 4 m). At a crest the
// wheel's cylinder cast touched the top edge but reported the steep side's normal; the disc placed against
// that side's plane, carried on past the edge, met it 39 cm up, the suspension length went to nothing and
// the physics engine's rigid stop threw the car up (the R34 at 5 m/s from 18 km/h, rolled at 90).
void TestWheelsRideOverSharpImpactBumps()
{
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    for (float z = 0.0f; z < 30.0f; z += 4.0f)
    {
        for (const auto& [x0, z0] : {std::pair{-0.2f, z}, {-1.9f, z + 2.93f}})
        {
            const float x1 = x0 + 2.1f;
            const uint32_t first = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), {
                                                {x0, 0.0f, z0}, {x1, 0.0f, z0}, {x1, 0.0f, z0 + 0.1663f}, {x0, 0.0f, z0 + 0.1663f},
                                                {x0 + 0.02f, 0.04f, z0 + 0.0282f}, {x1 - 0.02f, 0.04f, z0 + 0.0282f}, {x1 - 0.02f, 0.04f, z0 + 0.1382f},
                                                {x0 + 0.02f, 0.04f, z0 + 0.1382f},
                                            });
            for (const uint32_t index : {0u, 1u, 5u, 0u, 5u, 4u, 4u, 5u, 6u, 4u, 6u, 7u, 7u, 6u, 2u, 7u, 2u, 3u, 0u, 4u, 7u, 0u, 7u, 3u, 1u, 2u, 6u, 1u, 6u, 5u})
            {
                indices.push_back(first + index);
            }
        }
    }
    PhysicsWorld world;
    AddUpwardMesh(world, {{-50.0f, 0.0f, -300.0f}, {50.0f, 0.0f, -300.0f}, {50.0f, 0.0f, 100.0f}, {-50.0f, 0.0f, 100.0f}}, {0, 1, 2, 0, 2, 3});
    AddUpwardMesh(world, vertices, indices);
    const VehicleId car = world.AddVehicle(GtrSettings(), {glm::vec3(0.0f, 0.0f, -20.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    for (const float kmh : {15.0f, 30.0f, 60.0f})
    {
        world.ResetVehicle(car, {glm::vec3(0.0f, 0.0f, -40.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        constexpr float kFrame = 1.0f / 60.0f;
        VehicleControls controls;
        float fastestRise = 0.0f;
        float slowestKmh = 1000.0f;
        double lastY = world.GetVehiclePose(car).position.y;
        for (float time = 0.0f; time < 30.0f && world.GetVehiclePose(car).position.z < 32.0f; time += kFrame)
        {
            const float speed = world.GetVehicleTelemetry(car).forwardSpeed * 3.6f;
            controls.throttle = std::clamp((kmh - speed) * 0.2f, -1.0f, 1.0f);
            world.SetVehicleControls(car, controls);
            world.Update(kFrame);
            const PhysicsPose pose = world.GetVehiclePose(car);
            if (pose.position.z > -1.0f)
            {
                fastestRise = std::max(fastestRise, static_cast<float>(pose.position.y - lastY) / kFrame);
                slowestKmh = std::min(slowestKmh, world.GetVehicleTelemetry(car).forwardSpeed * 3.6f);
            }
            lastY = pose.position.y;
        }
        std::cout << "GT-R over 4 cm impact bumps at " << kmh << " km/h: body rose at up to " << fastestRise << " m/s, slowest " << slowestKmh
                  << " km/h, reached z " << world.GetVehiclePose(car).position.z << "\n";
        Require(world.GetVehiclePose(car).position.z > 32.0f, "the car drives over the bumps");
        Require(fastestRise < 0.5f, "the bumps do not throw the car");
    }
}

// Ground as generated heightfields and imported tracks often come: every 0.5 m cell a quad with its own
// four vertices, none shared with its neighbours: flat up to z = -40, then gentle waves (2 cm, 7.5 m long).
void AddGroundOfSeparateCells(PhysicsWorld& world)
{
    constexpr float kCell = 0.5f;
    const auto height = [](float z) { return z > -40.0f ? 0.02f * std::sin(2.0f * 3.14159265f * z / 7.5f) : 0.0f; };
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    for (float x = -10.0f; x < 10.0f; x += kCell)
    {
        for (float z = -200.0f; z < 40.0f; z += kCell)
        {
            const uint32_t first = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), {{x, height(z), z}, {x + kCell, height(z), z}, {x + kCell, height(z + kCell), z + kCell}, {x, height(z + kCell), z + kCell}});
            indices.insert(indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
        }
    }
    Require(world.AddStaticMesh(vertices, indices), "the ground builds");
}

// A splitter skimming the road (2 cm up here; Assetto Corsa's R34 has 6 cm, and the 3 cm waves of
// assets/scenes/rolling_road.yaml and a nose dipping under braking or over a crest take the rest) slides
// over the seams between the ground's cells. With nothing shared between them each seam was an open edge
// to the physics engine, whose contact normal there leans back as far as 25 degrees: at 90 km/h stopping
// the body's approach along it threw the R34 a metre and a half into the air and onto its roof. Welded
// into one surface, a seam between cells that meet almost flat takes its faces' normal.
void TestSplitterGlidesOverTheSeamsOfSeparateCells()
{
    PhysicsWorld world;
    AddGroundOfSeparateCells(world);
    VehicleSettings settings = GtrWithSplitter();
    const glm::vec3 centreOfMass = settings.chassisCenter + settings.centerOfMassOffset;
    settings.carColliders[1].center.y = 0.02f + 0.075f - centreOfMass.y;
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.0f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    constexpr float kFrame = 1.0f / 60.0f;
    VehicleControls controls;
    float fastestRise = 0.0f;
    float highest = -1.0f;
    float worstFrameLossKmh = 0.0f;
    float lastKmh = 0.0f;
    float atWavesKmh = 0.0f;
    float lastY = world.GetVehiclePose(car).position.y;
    for (float time = 0.0f; time < 30.0f && world.GetVehiclePose(car).position.z < 35.0f; time += kFrame)
    {
        // Up to 90 km/h on the flat, then on over the waves without throttle (the nose dips a little).
        const PhysicsPose before = world.GetVehiclePose(car);
        controls.throttle = before.position.z < -40.0f && world.GetVehicleTelemetry(car).forwardSpeed * 3.6f < 90.0f ? 1.0f : 0.0f;
        world.SetVehicleControls(car, controls);
        world.Update(kFrame);
        const PhysicsPose pose = world.GetVehiclePose(car);
        const float kmh = world.GetVehicleTelemetry(car).forwardSpeed * 3.6f;
        if (pose.position.z > -40.0f)
        {
            atWavesKmh = atWavesKmh == 0.0f ? kmh : atWavesKmh;
            fastestRise = std::max(fastestRise, static_cast<float>(pose.position.y - lastY) / kFrame);
            highest = std::max(highest, static_cast<float>(pose.position.y));
            worstFrameLossKmh = std::max(worstFrameLossKmh, lastKmh - kmh);
        }
        lastY = pose.position.y;
        lastKmh = kmh;
    }
    std::cout << "GT-R with a 2 cm splitter over waves of separate 0.5 m cells from " << atWavesKmh << " km/h: worst frame lost " << worstFrameLossKmh
              << " km/h, body rose at up to " << fastestRise << " m/s, highest " << highest << " m, " << lastKmh << " km/h after\n";
    Require(atWavesKmh > 85.0f, "the car reaches the waves at speed");
    Require(fastestRise < 1.0f, "the seams do not throw the car");
    Require(highest < 0.1f, "the car stays on the ground");
    Require(worstFrameLossKmh < 2.0f, "the splitter does not catch on the seams");
}

// The GT-R as the editor drives it: its data through ApplyCarSpec, then fitted to its model's bounds and
// wheel nodes (vehicle space). The body's centre of mass is the data's, not the model's middle: 55.5 % of
// the wheelbase ahead of the rear axle (CG_LOCATION) and 0.43 m up (tyre radius 0.355 - BASEY -0.075).
// As in Assetto Corsa that is the body's alone: the hubs (part of the total mass) sit at the wheels, so at
// rest the front tyres carry the body's 55.5 % and their own hubs.
void TestCarDataPlacesTheCentreOfMass()
{
    VehicleCarSpec spec = MakeGtrSpec();
    spec.wheelbase = 2.78f;
    spec.frontWeightShare = 0.555f;
    spec.inertiaBox = glm::vec3(1.8f, 1.35f, 4.8f);
    spec.frontSuspension->centerOfMassAboveWheel = 0.075f;
    spec.rearSuspension->centerOfMassAboveWheel = 0.075f;
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    const VehicleSettings settings =
        FitVehicleSettingsToBounds(glm::vec3(-1.031f, -0.066f, -2.401f), glm::vec3(1.031f, 1.357f, 2.453f), ApplyCarSpec(VehicleSettings{}, spec), &layout);
    const glm::vec3 com = settings.chassisCenter + settings.centerOfMassOffset;
    const float ground = 0.2919f - 0.355f;
    const float share = (com.z - settings.rearAxleZ) / (settings.frontAxleZ - settings.rearAxleZ);
    std::cout << "GT-R centre of mass from its data: " << share * 100.0f << " % front, " << (com.y - ground) * 1000.0f << " mm up\n";
    RequireNear(share, 0.555f, 0.002f, "the data's weight split");
    RequireNear(com.y - ground, 0.43f, 0.002f, "the data's centre of mass height");
    RequireNear(com.x, 0.0f, 1e-4f, "on the car's centre line");

    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls hold;
    hold.brake = 1.0f; // awake until the loads have settled
    world.SetVehicleControls(car, hold);
    Simulate(world, 3.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    const float front = wheels[0].suspensionForce + wheels[1].suspensionForce;
    const float total = front + wheels[2].suspensionForce + wheels[3].suspensionForce;
    const float hubs = 2.0f * (spec.frontSuspension->hubMass + spec.rearSuspension->hubMass);
    const float frontShare = ((settings.massKg - hubs) * 0.555f + 2.0f * spec.frontSuspension->hubMass) / settings.massKg;
    std::cout << "GT-R at rest: the front tyres carry " << front / total * 100.0f << " % of " << total << " N (expected "
              << frontShare * 100.0f << " %)\n";
    RequireNear(front / total, frontShare, 0.002f, "the front tyres carry the body's share and the front hubs");
    RequireNear(total, settings.massKg * 9.81f, 0.01f * settings.massKg * 9.81f, "and all of the weight");
}

// With Assetto Corsa's ROD_LENGTH the springs carry the car rodLength less than at the hardpoints' design
// position, so each wheel rests load / rate - rodLength of travel from it (the GT-R given 60 mm in front
// and 10 mm behind: front drooped, rear compressed). The car still rests where its model draws its wheels,
// with the body as it was: the design position is placed that far from them instead.
std::pair<std::vector<VehicleWheelState>, PhysicsPose> GtrAtRest(const VehicleCarSpec& spec, VehicleWheelLayout& layout, VehicleSettings& settings)
{
    layout[0] = {glm::vec3(0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    settings = FitVehicleSettingsToBounds(glm::vec3(-1.031f, -0.066f, -2.401f), glm::vec3(1.031f, 1.357f, 2.453f), ApplyCarSpec(VehicleSettings{}, spec), &layout);
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls hold;
    hold.brake = 1.0f;
    world.SetVehicleControls(car, hold);
    Simulate(world, 3.0f);
    return {world.GetVehicleWheels(car), world.GetVehiclePose(car)};
}

void TestRodLengthRestsWhereTheModelDrawsTheWheels()
{
    VehicleCarSpec plain = MakeGtrSpec();
    plain.wheelbase = 2.78f;
    plain.frontWeightShare = 0.555f;
    VehicleCarSpec rods = plain;
    rods.frontSuspension->rodLength = 0.06f;
    rods.rearSuspension->rodLength = 0.01f;
    VehicleWheelLayout layout{};
    VehicleSettings plainSettings;
    VehicleSettings settings;
    const auto [plainWheels, plainPose] = GtrAtRest(plain, layout, plainSettings);
    const auto [wheels, pose] = GtrAtRest(rods, layout, settings);
    const glm::quat toBody = glm::conjugate(pose.rotation);
    for (size_t index = 0; index < 4; ++index)
    {
        const VehicleSuspensionAxle& axle = index < 2 ? settings.frontSuspension : settings.rearSuspension;
        const double springLoad = wheels[index].suspensionForce - axle.hubMass * 9.81;
        const double expected = VehicleRestTravel(axle, springLoad);
        const glm::vec3 center = toBody * glm::vec3(wheels[index].pose.position - pose.position);
        std::cout << "GT-R with rod lengths, wheel " << index << ": travel " << wheels[index].travel * 1000.0f << " mm (springs give " << expected * 1000.0
                  << "), " << glm::length(center - layout[index].center) * 1000.0f << " mm from the model's wheel; camber " << wheels[index].camberDegrees
                  << " deg (" << plainWheels[index].camberDegrees << " at the design position)\n";
        RequireNear(wheels[index].travel, static_cast<float>(expected), 0.003f, "the wheel rests where its springs carry it");
        Require(std::abs(expected) > 0.005, "away from the design position");
        RequireNear(glm::length(center - layout[index].center), 0.0f, 0.003f, "where the model draws it");
    }
    Require(wheels[0].travel < 0.0f && wheels[2].travel > 0.0f, "the front drooped by its long rod, the rear compressed");
    RequireNear(pose.position.y, plainPose.position.y, 0.003f, "the body at the same height");
    RequireNear(glm::dot(pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                glm::dot(plainPose.rotation * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f)), 0.001f, "and the same attitude");
}

// The road's drive force on a rear wheel of the GT-R: with the brakes off its moment about the spin axis
// goes up the half shaft, so it loads the linkage by its work along the wheel centre's path (anti-squat);
// with the brakes holding it the knuckle takes it, and the work is along the contact point's path
// (anti-lift). The brakes hold no more than their torque.
void TestDriveForceLoadsTheLinkageAtTheWheelCentre()
{
    const VehicleSettings settings = GtrSettings();
    const VehicleCornerSetup setup = BuildVehicleCorner(settings, 2, 3000.0);
    suspension::SuspensionCorner corner(setup.definition, MakeVehicleCornerUnit(setup), suspension::SuspensionCorner::kWheelTravel);
    suspension::CornerInput in;
    in.travel = 0.02;
    corner.Step(in);
    const suspension::CornerOutput first = corner.Output();
    in.load.force = suspension::Vec3(2500.0, 0.0, 0.0); // forward, driving
    const auto loadOn = [&](double brakeTorque) {
        in.load.moment = KnuckleSpinMomentRelief(first.geometry, in.load.force, brakeTorque);
        return corner.Step(in).loadTravelForce;
    };
    const double driven = loadOn(0.0);
    const double braked = loadOn(1e5);
    const double halfHeld = loadOn(0.5 * 2500.0 * settings.wheelRadius);
    // Only the force in the wheel's plane has a moment about the spin axis; its part along the axis (toe
    // and camber) turns the knuckle about other axes, which the bearings hold.
    const suspension::Vec3 spin = first.geometry.spinAxis;
    const suspension::Vec3 alongAxis = glm::dot(in.load.force, spin) * spin;
    const double atCentre = glm::dot(in.load.force - alongAxis, first.wheelCenterPerTravel) + glm::dot(alongAxis, first.contactPerTravel);
    const double atContact = glm::dot(in.load.force, first.contactPerTravel);
    std::cout << "GT-R rear, 2.5 kN of drive at 20 mm bump: linkage load " << driven << " N (wheel centre path " << atCentre << "), braked " << braked << " N (contact path "
              << atContact << "), brakes holding half " << halfHeld << " N\n";
    RequireNear(static_cast<float>(driven), static_cast<float>(atCentre), 1.0f, "drive loads the linkage along the wheel centre's path");
    RequireNear(static_cast<float>(braked), static_cast<float>(atContact), 1.0f, "held by the brakes, along the contact point's path");
    Require(std::abs(atCentre - atContact) > 20.0, "the two paths differ on this linkage");
    RequireNear(static_cast<float>(halfHeld), static_cast<float>(0.5 * (driven + braked)), 0.05f * static_cast<float>(std::abs(driven - braked)), "brakes holding half the moment");
}

// Assetto Corsa's PACKER_RANGE: with a rod length the packers bring the bump stop in once the spring has
// compressed that far, rod length included, so the stop starts at packerRange - rodLength of travel when
// that comes before BUMPSTOP_UP; it may lie below the design position, the stop then pressing there.
// Without a rod length the packers are not read.
void TestPackersBringTheBumpStopIn()
{
    const VehicleSettings base = GtrSettings();
    const float bumpStop = base.frontSuspension.bumpStopTravel;
    Require(bumpStop > 0.03f && base.frontSuspension.bumpStopRate > 0.0f, "the GT-R has front bump stops");
    const auto stopAt = [&](std::optional<float> rod, std::optional<float> packers, double travel) {
        VehicleSettings settings = base;
        settings.frontSuspension.rodLength = rod;
        settings.frontSuspension.packerRange = packers;
        const std::optional<double> start = VehicleBumpStopStart(settings.frontSuspension);
        const double force = BuildVehicleCorner(settings, 0, 3000.0).unit.bumpStop.Value(travel);
        return std::pair<double, double>{start.value_or(-1.0), force};
    };
    const double rate = base.frontSuspension.bumpStopRate;
    RequireNear(static_cast<float>(stopAt(std::nullopt, 0.01f, 0.0).first), bumpStop, 1e-6f, "no rod length: the packers are not read");
    RequireNear(static_cast<float>(stopAt(0.06f, 0.5f, 0.0).first), bumpStop, 1e-6f, "packers past the bump stop change nothing");
    const auto [early, earlyForce] = stopAt(0.06f, 0.08f, 0.03);
    RequireNear(static_cast<float>(early), 0.02f, 1e-6f, "the packers bring the stop in to 20 mm of bump");
    RequireNear(static_cast<float>(earlyForce), static_cast<float>(rate * 0.01), 1e-3f * static_cast<float>(rate * 0.01), "and it presses from there");
    const auto [below, belowForce] = stopAt(0.06f, 0.04f, 0.0);
    RequireNear(static_cast<float>(below), -0.02f, 1e-6f, "packers below the design position");
    RequireNear(static_cast<float>(belowForce), static_cast<float>(rate * 0.02), 1e-3f * static_cast<float>(rate * 0.02), "press at the design position already");
    RequireNear(static_cast<float>(stopAt(0.06f, 0.04f, -0.03).second), 0.0f, 1e-6f, "and let go below their start");
}

// The GT-R's 30 litres of starting fuel (22.5 kg) in its tank 0.85 m behind and 0.15 m below the body's
// centre of mass: 1397.5 kg; the fuel joins the body (1375 kg less its 250 kg of hubs), whose share moves
// from 55.5 % to 54.9 % and centre of mass 2.9 mm lower. Applying it twice adds nothing, and the car at
// rest carries it.
void TestStartingFuelMovesTheMass()
{
    VehicleCarSpec spec = MakeGtrSpec();
    spec.wheelbase = 2.78f;
    spec.frontWeightShare = 0.555f;
    spec.frontSuspension->centerOfMassAboveWheel = 0.075f;
    spec.rearSuspension->centerOfMassAboveWheel = 0.075f;
    spec.fuelLitres = 30.0f;
    spec.fuelTankPosition = glm::vec3(0.0f, -0.15f, -0.85f);
    const VehicleCarSpec fuelled = WithStartingFuel(spec);
    const float fuel = 30.0f * kFuelKgPerLitre;
    RequireNear(*fuelled.massKg, 1375.0f + fuel, 1e-3f, "the fuel's mass");
    const float tankShare = 0.555f - 0.85f / 2.78f;
    const float hubs = 2.0f * (spec.frontSuspension->hubMass + spec.rearSuspension->hubMass);
    const float body = 1375.0f - hubs;
    RequireNear(*fuelled.frontWeightShare, (0.555f * body + tankShare * fuel) / (body + fuel), 1e-5f, "the body's weight split with the tank behind");
    RequireNear(fuelled.frontSuspension->centerOfMassAboveWheel, 0.075f - fuel * 0.15f / (body + fuel), 1e-6f, "the body's centre of mass lower");
    Require(!fuelled.fuelLitres.has_value(), "the fuel is counted once");
    const VehicleCarSpec twice = WithStartingFuel(fuelled);
    Require(*twice.massKg == *fuelled.massKg && *twice.frontWeightShare == *fuelled.frontWeightShare, "and applying it again adds nothing");
    std::cout << "GT-R with 30 L of fuel: " << *fuelled.massKg << " kg, " << *fuelled.frontWeightShare * 100.0f << " % front, centre of mass "
              << (0.355f + fuelled.frontSuspension->centerOfMassAboveWheel) * 1000.0f << " mm up\n";

    // Driven: the settings take the fuelled figures.
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    const VehicleSettings settings =
        FitVehicleSettingsToBounds(glm::vec3(-1.031f, -0.066f, -2.401f), glm::vec3(1.031f, 1.357f, 2.453f), ApplyCarSpec(VehicleSettings{}, spec), &layout);
    RequireNear(settings.massKg, 1375.0f + fuel, 1e-3f, "the car is driven with its fuel");
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls hold;
    hold.brake = 1.0f;
    world.SetVehicleControls(car, hold);
    Simulate(world, 3.0f);
    const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    const float front = wheels[0].suspensionForce + wheels[1].suspensionForce;
    const float total = front + wheels[2].suspensionForce + wheels[3].suspensionForce;
    const float frontShare = ((settings.massKg - hubs) * *fuelled.frontWeightShare + 2.0f * spec.frontSuspension->hubMass) / settings.massKg;
    RequireNear(front / total, frontShare, 0.002f, "the front tyres carry the fuelled body's share and the front hubs");
    RequireNear(total, settings.massKg * 9.81f, 0.01f * settings.massKg * 9.81f, "the tyres carry the fuel too");
}

// The hubs on their own tyre springs are part of the car's mass but sit at the wheels, not at the body's
// centre of mass, and the car moves as the two together. Held by its brakes on a slope falling towards its
// front (0.3 g along it, every mass loaded as braking would), the tyres' loads about that common centre of
// mass balance the car's weight along the slope at its height. The GT-R with its body's centre of mass
// raised to 0.6 m (hubs 250 of its 1375 kg, at 0.355 m): the common centre is about 0.555 m up.
void TestHubsLoadTheTyresFromTheWheels()
{
    VehicleCarSpec spec = MakeGtrSpec();
    spec.wheelbase = 2.78f;
    spec.frontWeightShare = 0.555f;
    spec.inertiaBox = glm::vec3(1.8f, 1.35f, 4.8f);
    spec.frontSuspension->centerOfMassAboveWheel = 0.245f;
    spec.rearSuspension->centerOfMassAboveWheel = 0.245f;
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[1] = {glm::vec3(-0.8583f, 0.2919f, 1.432f), 0.355f, 0.33f};
    layout[2] = {glm::vec3(0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    layout[3] = {glm::vec3(-0.869f, 0.2919f, -1.3454f), 0.355f, 0.33f};
    const VehicleSettings settings =
        FitVehicleSettingsToBounds(glm::vec3(-1.031f, -0.066f, -2.401f), glm::vec3(1.031f, 1.357f, 2.453f), ApplyCarSpec(VehicleSettings{}, spec), &layout);

    const float along = 0.3f;
    const glm::quat slope = glm::angleAxis(std::asin(along), glm::vec3(1.0f, 0.0f, 0.0f));
    std::vector<glm::vec3> vertices = {{-200.0f, 0.0f, -200.0f}, {-200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, 200.0f}, {200.0f, 0.0f, -200.0f}};
    for (glm::vec3& vertex : vertices)
    {
        vertex = slope * vertex;
    }
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    PhysicsWorld world;
    Require(world.AddStaticMesh(vertices, indices), "the slope builds");
    const VehicleId car = world.AddVehicle(settings, {slope * glm::vec3(0.0f, 0.05f, 0.0f), slope});
    VehicleControls hold;
    hold.brake = 1.0f;
    hold.handBrake = 1.0f;
    world.SetVehicleControls(car, hold);
    Simulate(world, 3.0f);

    // Where the body's centre of mass and the hubs (the wheels' centres) stand, in the slope's frame: down it
    // (the car's front) and up from it; and the tyres' loads about that common centre. The tyres' grip at a
    // standstill flickers from step to step, so these are the mean over half a second.
    const glm::vec3 down = slope * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 up = slope * glm::vec3(0.0f, 1.0f, 0.0f);
    const float hubs = 2.0f * (spec.frontSuspension->hubMass + spec.rearSuspension->hubMass);
    const float weight = settings.massKg * 9.81f;
    constexpr int kSteps = 500;
    float moment = 0.0f;
    float load = 0.0f;
    float height = 0.0f;
    float bodyHeight = 0.0f;
    for (int step = 0; step < kSteps; ++step)
    {
        world.Update(PhysicsWorld::kDefaultStepSeconds);
        const PhysicsPose pose = world.GetVehiclePose(car);
        const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
        const glm::dvec3 body = pose.position + glm::dvec3(pose.rotation * (settings.chassisCenter + settings.centerOfMassOffset));
        glm::dvec3 center = static_cast<double>(settings.massKg - hubs) * body;
        for (size_t index = 0; index < wheels.size(); ++index)
        {
            center += static_cast<double>(index < 2 ? spec.frontSuspension->hubMass : spec.rearSuspension->hubMass) * wheels[index].pose.position;
        }
        center /= static_cast<double>(settings.massKg);
        for (const VehicleWheelState& wheel : wheels)
        {
            moment += wheel.suspensionForce * glm::dot(glm::vec3(wheel.contactPosition - center), down) / kSteps;
            load += wheel.suspensionForce / kSteps;
        }
        height += static_cast<float>(glm::dot(center, glm::dvec3(up))) / kSteps;
        bodyHeight += static_cast<float>(glm::dot(body, glm::dvec3(up))) / kSteps;
    }
    const float shift = moment / (weight * along);
    std::cout << "GT-R braked on a 0.3 g slope: the tyres' loads balance it about the body and hubs' centre of mass as from " << shift * 1000.0f
              << " mm up; that centre is " << height * 1000.0f << " mm up, the body's alone " << bodyHeight * 1000.0f << " mm\n";
    RequireNear(load, weight * std::sqrt(1.0f - along * along), 0.01f * weight, "the tyres carry the car's weight into the slope");
    RequireNear(shift, height, 0.003f, "the car turns about the body and hubs' centre of mass");
}

// The AE86 on its own data: struts in front, a live axle behind. It rests on its design position; in a
// right-hand turn the body rolls onto its left wheels and the rear axle, one rigid beam, keeps both
// rear wheels upright to the road: against the body their cambers are equal and opposite, the
// compressed (left) wheel's top leaning in. The turn is a gentle one: on the default tyres' grip,
// which does not fall with load, this car (centre of mass 0.54 m up, 0.68 m half-track) lifts its
// inside wheels and rolls over near the limit.
void TestLiveAxleCarRestsAndCorners()
{
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.6775f, 0.2888f, 1.2f), 0.2888f, 0.185f};
    layout[1] = {glm::vec3(-0.6775f, 0.2888f, 1.2f), 0.2888f, 0.185f};
    layout[2] = {glm::vec3(0.675f, 0.2888f, -1.2f), 0.2888f, 0.185f};
    layout[3] = {glm::vec3(-0.675f, 0.2888f, -1.2f), 0.2888f, 0.185f};
    const VehicleSettings settings =
        FitVehicleSettingsToBounds(glm::vec3(-0.83f, 0.0f, -2.1f), glm::vec3(0.83f, 1.33f, 2.1f), ApplyCarSpec(VehicleSettings{}, MakeAe86Spec()), &layout);
    Require(HasSuspensionGeometry(settings) && settings.rearSuspension.type == VehicleSuspensionType::SolidAxle, "the AE86's live axle");

    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    VehicleControls controls;
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
    for (size_t index = 0; index < wheels.size(); ++index)
    {
        const std::string name = "wheel " + std::to_string(index);
        Require(wheels[index].multibody && wheels[index].inContact, name + " on its suspension and the ground");
        RequireNear(wheels[index].travel, 0.0f, 0.006f, name + ": travel at rest");
    }
    RequireNear(wheels[2].camberDegrees, 0.0f, 0.05f, "the rear axle's wheels upright at rest");
    RequireNear(wheels[3].camberDegrees, 0.0f, 0.05f, "both of them");
    std::cout << "AE86 at rest: travel FL " << wheels[0].travel * 1000.0f << " mm, RL " << wheels[2].travel * 1000.0f << " mm, RR "
              << wheels[3].travel * 1000.0f << " mm; camber FL " << wheels[0].camberDegrees << " deg\n";

    controls = VehicleControls{};
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 3.0f);
    controls.throttle = 0.3f;
    controls.steering = 0.15f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 2.0f);
    const PhysicsPose pose = world.GetVehiclePose(car);
    const glm::vec3 forward = pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    wheels = world.GetVehicleWheels(car);
    const float roll = RollDegrees(pose.rotation);
    std::cout << "AE86 turning at " << world.GetVehicleTelemetry(car).forwardSpeed << " m/s: roll " << roll << " deg; rear travel L "
              << wheels[2].travel * 1000.0f << " mm, R " << wheels[3].travel * 1000.0f << " mm; rear camber to the body L " << wheels[2].camberDegrees
              << ", R " << wheels[3].camberDegrees << " deg; rear toe L " << wheels[2].toeDegrees << ", R " << wheels[3].toeDegrees << " deg\n";
    Require(forward.x < -0.1f, "steering right turns right");
    Require(wheels[2].travel > wheels[3].travel + 0.003f, "the outer (left) rear is compressed");
    Require(wheels[2].camberDegrees < -0.1f, "the compressed rear wheel's top leans in against the body");
    RequireNear(wheels[2].camberDegrees + wheels[3].camberDegrees, 0.0f, 0.02f, "one beam: the rear cambers equal and opposite");
}

// The AE86's live axle under full throttle in first gear: the propshaft's torque twists the axle housing
// (TORQUE_REACTION -0.5), loading the left rear tyre and unloading the right, the body taking the
// opposite. Without the reaction the two rear tyres carry about the same.
float RearLoadSplitUnderPower(float torqueReaction)
{
    VehicleCarSpec spec = MakeAe86Spec();
    spec.rearSuspension->axleTorqueReaction = torqueReaction;
    VehicleWheelLayout layout{};
    layout[0] = {glm::vec3(0.6775f, 0.2888f, 1.2f), 0.2888f, 0.185f};
    layout[1] = {glm::vec3(-0.6775f, 0.2888f, 1.2f), 0.2888f, 0.185f};
    layout[2] = {glm::vec3(0.675f, 0.2888f, -1.2f), 0.2888f, 0.185f};
    layout[3] = {glm::vec3(-0.675f, 0.2888f, -1.2f), 0.2888f, 0.185f};
    const VehicleSettings settings =
        FitVehicleSettingsToBounds(glm::vec3(-0.83f, 0.0f, -2.1f), glm::vec3(0.83f, 1.33f, 2.1f), ApplyCarSpec(VehicleSettings{}, spec), &layout);
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(settings, {glm::vec3(0.0f, 0.05f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 2.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    // The left rear's load over the right's, averaged while the car pulls away.
    float split = 0.0f;
    int samples = 0;
    constexpr float kFrame = 1.0f / 144.0f;
    for (float time = 0.0f; time < 1.0f; time += kFrame)
    {
        world.Update(kFrame);
        if (time > 0.3f)
        {
            const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(car);
            split += wheels[2].suspensionForce - wheels[3].suspensionForce;
            ++samples;
        }
    }
    return split / static_cast<float>(std::max(samples, 1));
}

void TestLiveAxleTorqueReactionLoadsTheLeftRear()
{
    const float without = RearLoadSplitUnderPower(0.0f);
    const float with = RearLoadSplitUnderPower(-0.5f);
    std::cout << "AE86 pulling away: rear left minus right load " << without << " N without the axle's torque reaction, " << with << " N with it\n";
    Require(std::abs(without) < 60.0f, "without it the rear tyres share the load");
    Require(with - without > 100.0f, "the propshaft's torque loads the left rear and unloads the right");
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
    Require(applied.shiftUpRpm == 0.0f && applied.shiftDownRpm == 0.0f, "the shift points follow the revs");
    Require(applied.gearSwitchSeconds == 0.03f && applied.clutchReleaseSeconds == 0.1f && applied.engineInertia == 0.137f,
            "the clutch and the engine's inertia");
    Require(applied.tyres[0].longitudinalGrip == 1.314f && applied.tyres[2].inertia == 1.97f && applied.tyres[2].postPeakShare == 0.86f,
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

void TestErsAddsToTheEngineCurve()
{
    // Two curves add at every rpm either has, each read between its points and held past its ends.
    const std::vector<glm::vec2> sum = AddTorqueCurves({{1000.0f, 100.0f}, {3000.0f, 300.0f}}, {{0.0f, 50.0f}, {2000.0f, 50.0f}, {4000.0f, 0.0f}});
    Require(sum.size() == 5, "a point at every rpm of either");
    Require(sum[0] == glm::vec2(0.0f, 150.0f) && sum[1] == glm::vec2(1000.0f, 150.0f), "the first held below its start");
    RequireNear(sum[2].y, 200.0f + 50.0f, 1e-4f, "both read at 2000 rpm");
    RequireNear(sum[3].y, 300.0f + 25.0f, 1e-4f, "the second between its points");
    Require(sum[4] == glm::vec2(4000.0f, 300.0f), "the first held past its end");

    // A hybrid (the McLaren P1's figures): the motor's 260 Nm folds into the engine's curve, kept
    // apart too for a run-time ERS.
    VehicleCarSpec spec;
    spec.torqueCurve = {{1000.0f, 370.0f}, {4000.0f, 619.0f}, {8000.0f, 547.0f}, {9000.0f, 0.0f}};
    VehicleErs ers;
    ers.torqueCurve = {{0.0f, 260.0f}, {4000.0f, 260.0f}, {8000.0f, 178.0f}, {8300.0f, 0.0f}};
    spec.ers = ers;
    const VehicleSettings hybrid = ApplyCarSpec(VehicleSettings{}, spec);
    Require(hybrid.ersDelivery == VehicleErsDelivery::AddedToEngine && hybrid.ersTorqueCurve.size() == 4, "the motor's curve, delivered with the engine's");
    RequireNear(hybrid.maxEngineTorque, 619.0f + 260.0f, 1e-3f, "the peak is the engine's and the motor's");
    bool sawPeak = false;
    for (const glm::vec2& point : hybrid.torqueCurve)
    {
        if (point.x == 4000.0f)
        {
            RequireNear(point.y, 879.0f, 1e-3f, "879 Nm at 4000 rpm");
            sawPeak = true;
        }
        if (point.x == 9000.0f)
        {
            RequireNear(point.y, 0.0f, 1e-3f, "the motor stops past its curve");
        }
    }
    Require(sawPeak, "the engine's points are kept");

    // A car without one clears what an earlier hybrid left in the tuning.
    VehicleCarSpec plain;
    plain.torqueCurve = spec.torqueCurve;
    const VehicleSettings after = ApplyCarSpec(hybrid, plain);
    Require(after.ersDelivery == VehicleErsDelivery::None && after.ersTorqueCurve.empty() && after.maxEngineTorque == 619.0f, "no ERS, none delivered");
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

// On the road the GT-R changes up through its gears, changes down as it brakes to a stop and pulls away in
// first again: with its shift down at its idle the physics engine's box stayed in top and crawled off.
void TestCarChangesDownAsItStops()
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(GtrSettings(false), {glm::vec3(0.0f, 0.0f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    VehicleControls controls;
    controls.throttle = 1.0f;
    world.SetVehicleControls(car, controls);
    int highest = 0;
    for (int frame = 0; frame < 144 * 7; ++frame)
    {
        world.Update(1.0f / 144.0f);
        highest = std::max(highest, world.GetVehicleTelemetry(car).gear);
    }
    Require(highest >= 3, "full throttle changes up, reached gear " + std::to_string(highest));

    controls.throttle = 0.0f;
    controls.brake = 0.7f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 6.0f);
    VehicleTelemetry telemetry = world.GetVehicleTelemetry(car);
    Require(std::abs(telemetry.forwardSpeed) < 0.3f, "the car brakes to a stop, at " + std::to_string(telemetry.forwardSpeed) + " m/s");
    Require(telemetry.gear == 1, "stopped in first, gear " + std::to_string(telemetry.gear));

    controls.throttle = 1.0f;
    controls.brake = 0.0f;
    world.SetVehicleControls(car, controls);
    Simulate(world, 2.0f);
    telemetry = world.GetVehicleTelemetry(car);
    Require(telemetry.forwardSpeed > 30.0f / 3.6f, "and pulls away hard, " + std::to_string(telemetry.forwardSpeed * 3.6f) + " km/h in 2 s");
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
// `beforeBraking` runs at speed, just before the brakes go on.
void BrakeFromSpeed(
    const VehicleSettings& settings, float& lockedSeconds, float& distance, const std::function<void(PhysicsWorld&, VehicleId)>& beforeBraking = {})
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
    if (beforeBraking)
    {
        beforeBraking(world, car);
    }
    controls = {};
    controls.brake = 1.0f;
    world.SetVehicleControls(car, controls);
    const glm::dvec3 start = world.GetVehiclePose(car).position;
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
    for (VehicleTyreSettings& tyre : grippy.tyres)
    {
        tyre.longitudinalGrip = 2.2f;
    }
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

// Anti-lock brakes from the car's data: the brakes that locked the wheels above, with an ABS that lets a
// wheel's brake off past 12 % slip, looking 250 times a second, keep them turning and stop no later.
void TestAntiLockBrakesKeepTheWheelsTurning()
{
    VehicleSettings car = FitVehicleSettingsToBounds(kCarMin, kCarMax);
    car.dynamicBrakeBias = false;
    car.maxBrakeTorque = 1500.0f;
    car.frontBrakeShare = 0.5f;
    float lockedPlain = 0.0f;
    float distancePlain = 0.0f;
    BrakeFromSpeed(car, lockedPlain, distancePlain);
    car.absSlipRatioLimit = 0.12f;
    car.absRateHz = 250.0f;
    float lockedAbs = 0.0f;
    float distanceAbs = 0.0f;
    BrakeFromSpeed(car, lockedAbs, distanceAbs);
    std::cout << "25 m/s to rest: " << distancePlain << " m with " << lockedPlain << " s locked, with ABS " << distanceAbs << " m with " << lockedAbs << " s locked\n";
    Require(lockedPlain > 0.5f, "without ABS the wheels lock");
    Require(lockedAbs < 0.05f, "with it they keep turning, locked for " + std::to_string(lockedAbs) + " s");
    Require(distanceAbs <= distancePlain + 0.5f, "and the car stops no later");
    car.useAbs = false;
    float lockedOff = 0.0f;
    float distanceOff = 0.0f;
    BrakeFromSpeed(car, lockedOff, distanceOff);
    Require(lockedOff > 0.5f, "switched off, the car's ABS does nothing");

    // Switched while driving: off at speed it lets the wheels lock, and on again it keeps them turning.
    car.useAbs = true;
    float lockedSwitchedOff = 0.0f;
    float distanceSwitchedOff = 0.0f;
    BrakeFromSpeed(car, lockedSwitchedOff, distanceSwitchedOff, [](PhysicsWorld& world, VehicleId id)
                   {
                       world.SetVehicleDriverAids(id, false, true);
                   });
    Require(lockedSwitchedOff > 0.5f, "switched off while driving, the wheels lock, " + std::to_string(lockedSwitchedOff) + " s");
    car.useAbs = false;
    float lockedSwitchedOn = 0.0f;
    float distanceSwitchedOn = 0.0f;
    BrakeFromSpeed(car, lockedSwitchedOn, distanceSwitchedOn, [](PhysicsWorld& world, VehicleId id)
                   {
                       world.SetVehicleDriverAids(id, true, true);
                   });
    Require(lockedSwitchedOn < 0.05f, "switched on while driving, they keep turning, " + std::to_string(lockedSwitchedOn) + " s");
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

// `beforeLaunch` runs once the car has settled, just before the throttle goes down.
LaunchReport MeasureLaunch(const VehicleSettings& tuning, const std::function<void(PhysicsWorld&, VehicleId)>& beforeLaunch = {})
{
    PhysicsWorld world;
    AddGroundMesh(world);
    const VehicleId car = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, tuning), {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    Simulate(world, 1.0f);
    if (beforeLaunch)
    {
        beforeLaunch(world, car);
    }
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

    // The traction control switch turns the clutch's off too, at the start and while driving, and back on.
    tuning.useTractionControl = false;
    const LaunchReport switchedOff = MeasureLaunch(tuning);
    tuning.useTractionControl = true;
    const LaunchReport switchedOffWhileDriving = MeasureLaunch(tuning, [](PhysicsWorld& world, VehicleId id)
                                                               {
                                                                   world.SetVehicleDriverAids(id, true, false);
                                                               });
    tuning.useTractionControl = false;
    const LaunchReport switchedOnWhileDriving = MeasureLaunch(tuning, [](PhysicsWorld& world, VehicleId id)
                                                              {
                                                                  world.SetVehicleDriverAids(id, true, true);
                                                              });
    tuning.useTractionControl = true;
    std::cout << "launch, traction control switched off " << switchedOff.meanSlip << " mean, off while driving " << switchedOffWhileDriving.meanSlip
              << ", on while driving " << switchedOnWhileDriving.meanSlip << '\n';
    Require(std::abs(switchedOff.meanSlip - free.meanSlip) < 0.02f, "switched off, the tyres spin up as without it, " + std::to_string(switchedOff.meanSlip));
    Require(std::abs(switchedOffWhileDriving.meanSlip - free.meanSlip) < 0.02f, "switched off while driving, as well, " + std::to_string(switchedOffWhileDriving.meanSlip));
    Require(std::abs(switchedOnWhileDriving.meanSlip - held.meanSlip) < 0.02f, "switched on while driving, it holds them, " + std::to_string(switchedOnWhileDriving.meanSlip));

    tuning.limitedSlipDifferentials = false;
    tuning.tractionControlGrip = 0.0f; // with it holding both tyres to the ground, an open differential has nothing to show
    const LaunchReport open = MeasureLaunch(tuning);
    Require(held.meanGap < 0.03f, "the limited slip keeps the two rear wheels together, " + std::to_string(held.meanGap));
    // At 1000 Hz a level car launches almost symmetrically, so the open differential is judged against the limited slip.
    Require(open.meanGap > held.meanGap * 5.0f, "an open differential does not, " + std::to_string(open.meanGap));
}

// The Skyline R34's ctrl_4ws.ini (Super HICAS): the steering wheel's angle, scaled up past an oversteer
// factor of 1.2 and down from 130 km/h.
std::vector<VehicleController> MakeR34RearSteer()
{
    VehicleController steer{"STEER_DEG", "ADD", {{-90.0f, -0.0015f}, {-25.0f, -0.0010f}, {-10.0f, 0.0f}, {0.0f, 0.0f}, {10.0f, 0.0f}, {25.0f, 0.0010f}, {90.0f, 0.0015f}}, 0.99f, 1.0f, -1.0f};
    VehicleController oversteer{"OVERSTEER_FACTOR", "MULT", {{-1.6f, -2.0f}, {-1.2f, 1.0f}, {0.0f, 1.0f}, {1.2f, 1.0f}, {1.5f, 2.0f}}, 0.99f, 1.0f, -1.0f};
    VehicleController speed{"SPEED_KMH", "MULT", {{0.0f, 1.0f}, {130.0f, 1.0f}, {150.0f, 0.2f}}, 0.99f, 1.0f, -1.0f};
    return {steer, oversteer, speed};
}

void TestControllersCouplingAndBodyParts()
{
    // The R34's rear steering: 0.0015 rad at 90 degrees of steering wheel, a fifth of it at 150 km/h.
    const std::vector<VehicleController> hicas = MakeR34RearSteer();
    std::vector<float> filtered;
    VehicleControllerInputs inputs;
    inputs.steerDegrees = 90.0f;
    inputs.speedKmh = 100.0f;
    RequireNear(EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f), 0.0015f, 1e-7f, "0.0015 at 90 degrees");
    inputs.speedKmh = 150.0f;
    filtered.clear();
    RequireNear(EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f), 0.0003f, 1e-7f, "a fifth of it at 150 km/h");
    inputs.speedKmh = 100.0f;
    inputs.steerDegrees = 50.0f;
    filtered.clear();
    RequireNear(EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f), 0.0010f + 0.0005f * 25.0f / 65.0f, 1e-7f, "read between the curve's points");
    inputs.oversteerFactor = 1.5f;
    filtered.clear();
    RequireNear(EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f), 2.0f * (0.0010f + 0.0005f * 25.0f / 65.0f), 1e-7f, "doubled once the rear slides");
    Require(ComputeRearSteerAngle(0.0015f) == -0.0015f, "with the steering's sign it turns the rear against the front");
    Require(ReadVehicleControllerInput(inputs, "NOT_A_CHANNEL") == 0.0f, "an unknown input reads 0");

    // The filter: 0.99 a step at the game's 333 Hz is a time constant of 0.3 s. From straight ahead, 0.3 s
    // of 90 degrees reaches 1 - 1/e of the way.
    inputs = {};
    inputs.speedKmh = 100.0f;
    filtered.clear();
    EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f);
    inputs.steerDegrees = 90.0f;
    const float tau = -1.0f / (333.0f * std::log(0.99f));
    float value = 0.0f;
    const int steps = static_cast<int>(std::round(tau / 0.001f));
    for (int step = 0; step < steps; ++step)
    {
        value = EvaluateVehicleControllers(hicas, inputs, filtered, 0.001f);
    }
    RequireNear(value, 0.0015f * (1.0f - std::exp(-1.0f)), 0.0015f * 0.01f, "a time constant of 0.3 s");

    // ADD and MULT in order, each controller's limits held after it.
    const std::vector<VehicleController> limited = {{"GAS", "ADD", {{0.0f, 0.0f}, {1.0f, 10.0f}}, 0.0f, 4.0f, 0.0f}, {"GEAR", "MULT", {{0.0f, 3.0f}, {6.0f, 3.0f}}, 0.0f, 100.0f, 0.0f}};
    inputs = {};
    inputs.gas = 1.0f;
    filtered.clear();
    RequireNear(EvaluateVehicleControllers(limited, inputs, filtered, 0.0f), 12.0f, 1e-5f, "10 held to 4, then times 3");

    // The centre coupling: 100 Nm per rad/s of shaft slip, at most 1000 Nm, the shafts final-drive times the
    // wheels.
    RequireNear(ComputeCentreCouplingTorque(100.0f, 1000.0f, 3.545f, 11.0f, 10.0f), 354.5f, 1e-2f, "354.5 Nm for 1 rad/s at the wheels");
    RequireNear(ComputeCentreCouplingTorque(100.0f, 1000.0f, 3.545f, 20.0f, 10.0f), 1000.0f, 1e-3f, "held at its limit");
    RequireNear(ComputeCentreCouplingTorque(100.0f, 1000.0f, 3.545f, 10.0f, 10.5f), -177.25f, 1e-2f, "and back when the front turns faster");

    // The body: the boxes from the centre of mass, the shell's low points raised to the boxes' top.
    VehicleSettings body;
    body.chassisCenter = glm::vec3(0.0f, 0.8f, 0.1f);
    body.centerOfMassOffset = glm::vec3(0.0f, -0.3f, -0.1f); // centre of mass (0, 0.5, 0)
    body.carColliders = {{glm::vec3(0.0f, -0.23f, -0.9f), glm::vec3(1.75f, 0.15f, 3.0f), true}, {glm::vec3(0.0f, -0.38f, 1.8f), glm::vec3(1.57f, 0.15f, 0.35f), true}};
    body.chassisHull = {{1.0f, 0.1f, 2.0f}, {-1.0f, 1.3f, -2.0f}};
    const VehicleChassisParts parts = BuildChassisParts(body);
    Require(parts.boxes.size() == 2 && parts.hull.size() == 2, "two boxes and the shell");
    RequireNear(parts.boxes[0].center.y, 0.27f, 1e-5f, "a box centred from the centre of mass");
    RequireNear(parts.boxes[0].halfExtents.z, 1.5f, 1e-5f, "half its size");
    RequireNear(parts.hull[0].y, 0.5f - 0.23f + 0.075f, 1e-5f, "the shell's floor raised to the highest box top");
    RequireNear(parts.hull[1].y, 1.3f, 1e-6f, "its roof kept");
    Require(BuildChassisParts(VehicleSettings{}).boxes.empty(), "no boxes, no parts");
}

// Moving off, the clutch slips on the engine's revs: open below 95 % of the launch rpm, shut at 115 %, the
// point scaled from the idle by the throttle; the launch is over once the clutch is shut or the wheels turn
// the engine at its speed. An upshift also comes on the engine's revs when the wheels lag them.
void TestGearboxLaunchesOnTheEnginesRevs()
{
    VehicleGearbox gearbox = GtrGearbox(); // idle 2100 rpm
    gearbox.launchRpm = 4000.0f;
    const float dt = 1.0f / 1000.0f;
    VehicleGearboxState state;
    SettleGearbox(gearbox, state, 0.0f, 0.0f);
    Require(state.idling, "at rest the clutch is open");
    UpdateAutomaticGearbox(gearbox, state, 1.0f, 0.0f, dt, 2100.0f);
    Require(state.launching && state.gear == 1 && state.clutch == 0.0f, "on the throttle at the idle it launches, the clutch open");
    UpdateAutomaticGearbox(gearbox, state, 1.0f, 0.0f, dt, 0.95f * 4000.0f);
    Require(state.clutch == 0.0f, "still open at 95 % of the launch rpm");
    UpdateAutomaticGearbox(gearbox, state, 1.0f, 0.0f, dt, 4200.0f);
    RequireNear(state.clutch, 0.5f, 1e-4f, "half shut halfway up the window");
    Require(state.launching, "still launching");
    UpdateAutomaticGearbox(gearbox, state, 1.0f, 500.0f, dt, 1.15f * 4000.0f);
    Require(!state.launching && state.clutch == 1.0f, "shut at 115 %: the launch is over");

    // Half throttle launches halfway between the idle and the launch rpm.
    VehicleGearboxState gentle;
    SettleGearbox(gearbox, gentle, 0.0f, 0.0f);
    UpdateAutomaticGearbox(gearbox, gentle, 0.5f, 0.0f, dt, 2500.0f);
    const float point = 2100.0f + 0.5f * (4000.0f - 2100.0f);
    UpdateAutomaticGearbox(gearbox, gentle, 0.5f, 0.0f, dt, 0.5f * (0.95f + 1.15f) * point);
    RequireNear(gentle.clutch, 0.5f, 1e-4f, "half throttle's window is nearer the idle");
    // The wheels catching up with the engine end it too.
    UpdateAutomaticGearbox(gearbox, gentle, 0.5f, 3200.0f / gearbox.forwardRatios[0], dt, 3220.0f);
    Require(!gentle.launching && gentle.clutch == 1.0f, "the wheels turning the engine at its speed end it");
    // Off the throttle it stops launching.
    VehicleGearboxState lifted;
    SettleGearbox(gearbox, lifted, 0.0f, 0.0f);
    UpdateAutomaticGearbox(gearbox, lifted, 1.0f, 0.0f, dt, 2100.0f);
    UpdateAutomaticGearbox(gearbox, lifted, 0.0f, 0.0f, dt, 3000.0f);
    Require(!lifted.launching && lifted.clutch == 0.0f, "lifting ends it, the clutch open");
    // Without a launch rpm the clutch bites over its release time, as before.
    VehicleGearbox plain = GtrGearbox();
    VehicleGearboxState plainState;
    SettleGearbox(plain, plainState, 0.0f, 0.0f);
    UpdateAutomaticGearbox(plain, plainState, 1.0f, 0.0f, dt, 2100.0f);
    Require(!plainState.launching && plainState.clutch > 0.0f && plainState.clutch < 0.1f, "no launch rpm: the clutch starts to bite");

    // Up on the engine's revs when the wheels lag them (the clutch slipping): first gear's wheels at 6400 rpm,
    // the engine at the point.
    VehicleGearboxState lagging;
    lagging.gear = 1;
    const float output = 6400.0f / gearbox.forwardRatios[0];
    for (int step = 0; step < 1000; ++step)
    {
        UpdateAutomaticGearbox(gearbox, lagging, 1.0f, output, dt, 6400.0f);
    }
    Require(lagging.gear == 1, "below the point it holds first, " + std::to_string(gearbox.shiftPoints.upFull));
    UpdateAutomaticGearbox(gearbox, lagging, 1.0f, output, dt, gearbox.shiftPoints.upFull + 10.0f);
    Require(lagging.gear == 2, "the engine past the point changes up though the wheels are below it");

    // The car's own automatic gearbox's point is where it changes up on full throttle.
    VehicleCarSpec spec = MakeGtrSpec();
    spec.autoShiftUpRpm = 6800.0f;
    spec.clutchReleaseSeconds = 0.0f;
    const VehicleSettings applied = ApplyCarSpec(VehicleSettings{}, spec);
    Require(applied.shiftUpRpm == 6800.0f && ComputeVehicleShiftPoints(applied).upFull == 6800.0f, "the data's change point");
    Require(applied.clutchReleaseSeconds == 0.0f, "a clutch that bites at once");
    spec.autoShiftUpRpm = 9000.0f; // past the limiter
    Require(ApplyCarSpec(VehicleSettings{}, spec).shiftUpRpm == 0.0f, "a point past the limiter is ignored");
}

// Launching on the throttle the engine holds near the launch rpm while the clutch slips, and a car whose
// tyres can take the torque (four-wheel drive, as the R34) gets away faster than from the idle. A rear-drive
// car that its tyres hold back gains nothing: traction control slips the clutch either way, and the revs
// take a moment to rise first.
void TestLaunchHoldsTheRevs()
{
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    tuning.drive = VehicleDrive::AllWheel;
    tuning.centreDrive = VehicleCentreDrive::Coupling;
    tuning.centreCouplingRampTorque = 100.0f;
    tuning.centreCouplingMaxTorque = 1000.0f;
    const auto launch = [&](float launchRpm, float& lowRpm, float& highRpm)
    {
        VehicleSettings car = tuning;
        car.launchRpm = launchRpm;
        PhysicsWorld world;
        AddGroundMesh(world);
        const VehicleId id = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, car), {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(id, controls);
        lowRpm = 1e9f;
        highRpm = 0.0f;
        float t = 0.0f;
        while (world.GetVehicleTelemetry(id).forwardSpeed < 60.0f / 3.6f && t < 10.0f)
        {
            world.Update(1.0f / 100.0f);
            t += 1.0f / 100.0f;
            if (t > 0.3f && t < 0.6f)
            {
                lowRpm = std::min(lowRpm, world.GetVehicleTelemetry(id).engineRpm);
                highRpm = std::max(highRpm, world.GetVehicleTelemetry(id).engineRpm);
            }
        }
        return t;
    };
    float idleLow = 0.0f;
    float idleHigh = 0.0f;
    float launchLow = 0.0f;
    float launchHigh = 0.0f;
    const float fromIdle = launch(0.0f, idleLow, idleHigh);
    const float launched = launch(4000.0f, launchLow, launchHigh);
    std::cout << "0-60 km/h from the idle " << fromIdle << " s (" << idleLow << ".." << idleHigh << " rpm), launching at 4000 rpm " << launched << " s (" << launchLow << ".."
              << launchHigh << " rpm)\n";
    Require(launchLow > 0.9f * 4000.0f && launchHigh < 1.2f * 4000.0f, "the engine holds near the launch rpm while the clutch slips");
    Require(launched < fromIdle, "and the car gets away faster");
}

// A coupled four-wheel drive (the R34's AWD2: 100 Nm per rad/s, 1000 Nm) launching with its rear tyres
// slipping: the coupling passes torque and the front tyres drive, where on rear drive alone they only roll.
void TestCentreCouplingDrivesTheFront()
{
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    tuning.tractionControlGrip = 0.0f;
    const auto launch = [&](const VehicleSettings& car, float& meanFrontForce, float& meanCoupling)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        const VehicleId id = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, car), {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 1.0f;
        world.SetVehicleControls(id, controls);
        meanFrontForce = 0.0f;
        meanCoupling = 0.0f;
        const int frames = 2 * 60;
        for (int frame = 0; frame < frames; ++frame)
        {
            world.Update(1.0f / 60.0f);
            const std::vector<VehicleWheelState> wheels = world.GetVehicleWheels(id);
            meanFrontForce += 0.5f * (wheels[0].longitudinalForce + wheels[1].longitudinalForce) / frames;
            meanCoupling += world.GetVehicleTelemetry(id).centreCouplingTorque / frames;
        }
        return world.GetVehicleTelemetry(id).forwardSpeed;
    };
    float rearFront = 0.0f;
    float rearCoupling = 0.0f;
    const float rearSpeed = launch(tuning, rearFront, rearCoupling);
    VehicleSettings coupled = tuning;
    coupled.drive = VehicleDrive::AllWheel;
    coupled.centreDrive = VehicleCentreDrive::Coupling;
    coupled.centreCouplingRampTorque = 100.0f;
    coupled.centreCouplingMaxTorque = 1000.0f;
    coupled.axleDifferentials = {VehicleAxleDifferential{0.03f, 0.0f}, VehicleAxleDifferential{0.6f, 10.0f}};
    float coupledFront = 0.0f;
    float coupledCoupling = 0.0f;
    const float coupledSpeed = launch(coupled, coupledFront, coupledCoupling);
    std::cout << "launch for 2 s: rear drive " << rearSpeed << " m/s, front tyres " << rearFront << " N; coupled AWD " << coupledSpeed << " m/s, front tyres "
              << coupledFront << " N, coupling " << coupledCoupling << " Nm\n";
    Require(rearCoupling == 0.0f && rearFront < 50.0f, "on rear drive the front tyres only roll");
    Require(coupledCoupling > 50.0f, "the coupling passes torque while the rear slips, " + std::to_string(coupledCoupling));
    Require(coupledFront > 300.0f, "and the front tyres drive, " + std::to_string(coupledFront));
    Require(coupledSpeed > rearSpeed, "the coupled car gets away faster");
}

// The R34's Super HICAS: steering right at 60 km/h turns the rear wheels a fraction of a degree left,
// against the front, and the car turns in a little harder for it.
void TestRearSteerTurnsAgainstTheFrontAtSpeed()
{
    VehicleSettings tuning = ApplyCarSpec(VehicleSettings{}, MakeBoxsterSpec());
    const auto corner = [&](const VehicleSettings& car, float& rearSteerDegrees, float& yawRate)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        const VehicleId id = world.AddVehicle(FitVehicleSettingsToBounds(kCarMin, kCarMax, car), {glm::vec3(0.0f, 0.3f, -190.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        Simulate(world, 1.0f);
        VehicleControls controls;
        controls.throttle = 0.6f;
        world.SetVehicleControls(id, controls);
        while (world.GetVehicleTelemetry(id).forwardSpeed < 60.0f / 3.6f)
        {
            world.Update(1.0f / 60.0f);
        }
        controls.throttle = 0.15f;
        controls.steering = 0.1f;
        world.SetVehicleControls(id, controls);
        glm::quat before = world.GetVehiclePose(id).rotation;
        for (int frame = 0; frame < 90; ++frame)
        {
            before = world.GetVehiclePose(id).rotation;
            world.Update(1.0f / 60.0f);
        }
        const glm::quat after = world.GetVehiclePose(id).rotation;
        yawRate = std::abs(glm::eulerAngles(glm::conjugate(before) * after).y) * 60.0f;
        rearSteerDegrees = world.GetVehicleTelemetry(id).rearSteerDegrees;
    };
    float plainSteer = 0.0f;
    float plainYaw = 0.0f;
    corner(tuning, plainSteer, plainYaw);
    VehicleSettings hicas = tuning;
    hicas.rearSteerControllers = MakeR34RearSteer();
    hicas.steeringWheelLockDegrees = 450.0f;
    float hicasSteer = 0.0f;
    float hicasYaw = 0.0f;
    corner(hicas, hicasSteer, hicasYaw);
    // 45 degrees of steering wheel: 0.0010 + 0.0005 * 20 / 65 rad, against the front (left of a right turn).
    const float expected = -(0.0010f + 0.0005f * 20.0f / 65.0f) * 180.0f / 3.14159265f;
    std::cout << "rear steer at 60 km/h, 45 deg of steering wheel: " << hicasSteer << " deg (expected " << expected << "), yaw rate " << hicasYaw << " rad/s against "
              << plainYaw << " without\n";
    Require(plainSteer == 0.0f, "no controllers, no rear steer");
    RequireNear(hicasSteer, expected, std::abs(expected) * 0.3f, "the rear wheels turn against the front");
    Require(hicasYaw > plainYaw, "and the car turns in harder");
}

// The car's own body: its boxes and its shell make the collision shape, the shell's floor raised to the
// boxes' top so that only the boxes can meet the ground; at ride height the boxes clear it, and the car
// settles where it would on the plain chassis box.
void TestCarBodyIsItsBoxesAndShell()
{
    VehicleSettings tuning;
    tuning.carColliders = {{glm::vec3(0.0f, -0.23f, -0.9f), glm::vec3(1.75f, 0.15f, 3.0f), true}, {glm::vec3(0.0f, -0.26f, 1.1f), glm::vec3(1.75f, 0.15f, 1.0f), true}};
    for (const float x : {kCarMin.x, kCarMax.x})
    {
        for (const float y : {kCarMin.y, kCarMax.y})
        {
            for (const float z : {kCarMin.z, kCarMax.z})
            {
                tuning.chassisHull.emplace_back(x, y, z);
            }
        }
    }
    const auto build = [&](const VehicleSettings& car, std::pair<glm::vec3, glm::vec3>& bounds, glm::vec3& centreOfMass)
    {
        PhysicsWorld world;
        AddGroundMesh(world);
        const VehicleSettings fitted = FitVehicleSettingsToBounds(kCarMin, kCarMax, car);
        centreOfMass = fitted.chassisCenter + fitted.centerOfMassOffset;
        const VehicleId id = world.AddVehicle(fitted, {glm::vec3(0.0f, 0.3f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
        bounds = world.GetVehicleBodyBounds(id);
        Simulate(world, 2.0f);
        const PhysicsPose pose = world.GetVehiclePose(id);
        return static_cast<float>((pose.position + glm::dvec3(pose.rotation * centreOfMass)).y);
    };
    std::pair<glm::vec3, glm::vec3> plainBounds;
    std::pair<glm::vec3, glm::vec3> ownBounds;
    std::pair<glm::vec3, glm::vec3> floorBounds;
    glm::vec3 com(0.0f);
    const float plainRest = build(VehicleSettings{}, plainBounds, com);
    const float ownRest = build(tuning, ownBounds, com);
    VehicleSettings floorOnly = tuning;
    floorOnly.chassisHull.clear();
    build(floorOnly, floorBounds, com);
    std::cout << "body bounds: boxes and shell y " << ownBounds.first.y << ".." << ownBounds.second.y << ", boxes alone up to " << floorBounds.second.y
              << "; centre of mass at rest " << ownRest << " m (plain box " << plainRest << ")\n";
    RequireNear(ownBounds.first.y, com.y - 0.26f - 0.075f, 0.005f, "the lowest box's floor is the body's lowest point");
    RequireNear(ownBounds.second.y, kCarMax.y, 0.005f, "the shell's roof its highest");
    RequireNear(ownBounds.first.z, std::min(kCarMin.z, com.z - 0.9f - 1.5f), 0.005f, "the longer of the shell and the floor box its tail");
    RequireNear(floorBounds.second.y, com.y - 0.23f + 0.075f, 0.005f, "the boxes alone reach their own tops");
    RequireNear(ownRest, plainRest, 0.01f, "at ride height the boxes clear the ground");
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
        TestShiftPointsFollowTheRevRange();
        TestGearboxPicksTheGearBySpeedAndThrottle();
        TestGearboxChangeTimesAndUpshiftCut();
        TestCarSettlesAfterBrakingToAStop();
        TestGearboxDoesNotHunt();
        TestManualGearboxChangesWhenAsked();
        TestAutomaticGearboxHoldsThePaddlesGear();
        TestUpdateRunsFixedSteps();
        TestStepIsSettable();
        TestCarDrivesAtAnyStepRate();
        TestGroundCoverIsRecognised();
        TestGrassCardsStopACarUnlessTheyAreGroundCover();
        TestSurfaceFrictionSetsGrip();
        TestCarScrapingAWallStaysOnTheGround();
        TestDegenerateMeshIsRejected();
        TestAutomaticBrakesDoNotLockTheWheels();
        TestAntiLockBrakesKeepTheWheelsTurning();
        TestBrakeTorqueFollowsTheLoad();
        TestCarSpecReplacesWhatItKnows();
        TestErsAddsToTheEngineCurve();
        TestControllersCouplingAndBodyParts();
        TestGearboxLaunchesOnTheEnginesRevs();
        TestLaunchHoldsTheRevs();
        TestCentreCouplingDrivesTheFront();
        TestRearSteerTurnsAgainstTheFrontAtSpeed();
        TestCarBodyIsItsBoxesAndShell();
        TestCarOnItsOwnDataAccelerates();
        TestCarChangesDownAsItStops();
        TestTyreGripSetsAcceleration();
        TestAerodynamicsDragsAndPressesDown();
        TestWaterSurfaceHeights();
        TestCarFloatsThenSinksInWater();
        TestCarInATunnelUnderTheWaterStaysDry();
        TestCarSettlesOnTheGround();
        TestCarRecoversFromItsRoof();
        TestCarDrivesSteersAndReverses();
        TestCarDrivesOnTheManualGearbox();
        TestCarRotatedAtStartDrivesItsOwnWay();
        TestFitUsesTheModelsWheels();
        TestCarSitsOnTheModelsWheels();
        TestSpringsSettleAtTheRestLength();
        TestWheelStateReportsTyrePhysics();
        TestFastWheelsRollTheRightWay();
        TestEngineBrakingFromTheData();
        TestTurboSpoolsWithItsLag();
        TestCarDataGivesDifferentialAndTyreSensitivity();
        TestGameTractionControlCutsTheThrottle();
        TestDrivenWheelsKeepNearTheGround();
        TestMultibodyCarRestsAtItsDesignPosition(false);
        TestMultibodyCarRestsAtItsDesignPosition(true);
        TestUnsprungCarStandsOnItsTyres();
        TestUnsprungWheelsHangInTheAirAndLand();
        TestHardLandingIsCushioned();
        TestUnsprungCarTakesABump();
        TestSplitterRidesOverARoadSpike();
        TestWheelMountsATallKerbWithoutLeaping();
        TestWheelsRideOverSharpImpactBumps();
        TestSplitterGlidesOverTheSeamsOfSeparateCells();
        TestCarDataPlacesTheCentreOfMass();
        TestRodLengthRestsWhereTheModelDrawsTheWheels();
        TestDriveForceLoadsTheLinkageAtTheWheelCentre();
        TestPackersBringTheBumpStopIn();
        TestStartingFuelMovesTheMass();
        TestHubsLoadTheTyresFromTheWheels();
        TestLiveAxleCarRestsAndCorners();
        TestLiveAxleTorqueReactionLoadsTheLeftRear();
        TestMultibodyCarCornersOnItsLinkage(false);
        TestMultibodyCarCornersOnItsLinkage(true);
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
