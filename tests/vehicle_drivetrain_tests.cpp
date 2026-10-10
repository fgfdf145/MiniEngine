// The drivetrain's own step (engine/physics/vehicle_drivetrain.h): the belt's twist on its rim, the
// clutch and the brakes as dry friction, and the step's stability at the slowest physics rate.

#include <engine/physics/vehicle_drivetrain.h>

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace me;

namespace
{
void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

void RequireNear(double value, double expected, double tolerance, const std::string& what)
{
    Require(std::abs(value - expected) <= tolerance,
            what + ": " + std::to_string(value) + ", expected " + std::to_string(expected) + " within " + std::to_string(tolerance));
}

std::array<DrivetrainWheel, kDrivetrainWheels> Wheels(double rim = 0.8, double belt = 0.8, double stiffness = 70000.0, double damping = 0.0)
{
    std::array<DrivetrainWheel, kDrivetrainWheels> wheels{};
    for (DrivetrainWheel& wheel : wheels)
    {
        wheel = {rim, belt, stiffness, damping};
    }
    return wheels;
}

// A belt twisted on a rim that cannot turn rings at sqrt(k / I_belt).
void TestBeltRingsOnItsSidewall()
{
    const auto wheels = Wheels(1.0e9, 0.8, 70000.0, 0.0);
    DrivetrainState state;
    state.twist[0] = 0.01;
    DrivetrainStep step;
    step.dt = 1.0e-4;
    int crossings = 0;
    double firstCrossing = -1.0;
    double lastCrossing = 0.0;
    double previous = state.twist[0];
    double largest = 0.0;
    for (int index = 1; index <= 5000; ++index)
    {
        StepDrivetrain(wheels, state, step);
        const double twist = state.twist[0];
        largest = std::max(largest, std::abs(twist));
        if ((twist > 0.0) != (previous > 0.0))
        {
            const double time = index * step.dt;
            firstCrossing = firstCrossing < 0.0 ? time : firstCrossing;
            lastCrossing = time;
            ++crossings;
        }
        previous = twist;
    }
    const double frequency = 0.5 * (crossings - 1) / (lastCrossing - firstCrossing);
    const double expected = std::sqrt(70000.0 / 0.8) / (2.0 * std::numbers::pi);
    RequireNear(frequency, expected, 0.01 * expected, "the belt's twist rings at its own frequency");
    Require(largest <= 0.01 * 1.0001, "an undamped twist never grows, " + std::to_string(largest));
    std::cout << "belt on a fixed rim: " << frequency << " Hz (expected " << expected << ")\n";
}

// Rim and belt alone pass their spin back and forth and keep it all.
void TestSpinIsKept()
{
    const auto wheels = Wheels(0.6, 0.9, 70000.0, 3.0);
    DrivetrainState state;
    state.rim[1] = 10.0;
    DrivetrainStep step;
    const double before = 0.6 * state.rim[1] + 0.9 * state.belt[1];
    for (int index = 0; index < 2000; ++index)
    {
        StepDrivetrain(wheels, state, step);
    }
    const double after = 0.6 * state.rim[1] + 0.9 * state.belt[1];
    RequireNear(after, before, 1e-9 * before, "rim and belt keep their spin");
    RequireNear(state.rim[1], state.belt[1], 0.01, "the damped twist settles the two together");
}

DrivetrainStep RearDrive(double ratio, double capacity, double engineTorque)
{
    DrivetrainStep step;
    step.engineInertia = 0.2;
    step.engineTorque = engineTorque;
    step.clutchCapacity = capacity;
    step.clutchWeights = {0.0, 0.0, 0.5 * ratio, 0.5 * ratio};
    return step;
}

// A clutch inside its capacity turns the engine and the wheels as one: the engine's speed stays the
// gearing's of the rims', and the whole accelerates by its torque over the inertia it sees.
void TestClutchGrips()
{
    const auto wheels = Wheels(0.8, 0.8, 70000.0, 5.0);
    DrivetrainState state;
    const double ratio = 10.0;
    const DrivetrainStep step = RearDrive(ratio, 1000.0, 200.0);
    DrivetrainResult result;
    for (int index = 0; index < 1000; ++index)
    {
        result = StepDrivetrain(wheels, state, step);
        Require(result.clutchLocked, "the clutch grips inside its capacity");
        RequireNear(state.engine, 0.5 * ratio * (state.rim[2] + state.rim[3]), 1e-9 * std::max(state.engine, 1.0), "the engine turns with the rims");
    }
    // After a second, the inertia at the engine: its own and the two driven wheels' through the gearing.
    const double inertia = 0.2 + 2.0 * 1.6 / (ratio * ratio);
    RequireNear(state.engine, 200.0 / inertia, 0.01 * 200.0 / inertia, "engine and wheels accelerate together");
    Require(std::abs(state.rim[0]) < 1e-12, "an undriven wheel stays still");
}

// Past its capacity the clutch slips and passes its capacity: here against wheels held on their brakes,
// which then take what it passes.
void TestClutchSlips()
{
    const auto wheels = Wheels();
    DrivetrainState state;
    DrivetrainStep step = RearDrive(10.0, 100.0, 300.0);
    step.brakeCapacity = {5000.0, 5000.0, 5000.0, 5000.0};
    const DrivetrainResult result = StepDrivetrain(wheels, state, step);
    Require(result.brakeLocked[2] && result.brakeLocked[3], "the brakes hold the driven wheels");
    RequireNear(result.brakeTorque[2] + result.brakeTorque[3], -100.0 * 10.0, 1e-6, "the brakes take what the clutch passes, geared");
    Require(!result.clutchLocked, "the clutch slips past its capacity");
    RequireNear(result.clutchTorque, 100.0, 1e-9, "a slipping clutch passes its capacity");
    RequireNear(state.engine, (300.0 - 100.0) / 0.2 * step.dt, 1e-12, "the engine gains what the clutch does not pass");
}

// Brakes hold a road torque under their capacity, the rim still and the belt twisted to carry it, and
// let it turn past it.
void TestBrakesHoldAndSlip()
{
    // Damped to about 0.1 of critical, so the held belt has settled within the two seconds.
    const auto wheels = Wheels(0.8, 0.8, 70000.0, 50.0);
    DrivetrainState held;
    DrivetrainStep step;
    step.beltTorque[0] = 50.0;
    step.brakeCapacity[0] = 100.0;
    DrivetrainResult result;
    for (int index = 0; index < 2000; ++index)
    {
        result = StepDrivetrain(wheels, held, step);
    }
    Require(result.brakeLocked[0], "the brake holds under its capacity");
    RequireNear(held.rim[0], 0.0, 1e-12, "the held rim stands still");
    RequireNear(held.belt[0], 0.0, 1e-6, "the belt settles");
    RequireNear(held.twist[0], -50.0 / 70000.0, 1e-7, "the sidewall carries the road's torque");
    RequireNear(result.brakeTorque[0], -50.0, 1e-3, "the brake takes the road's torque");

    DrivetrainState sliding;
    step.brakeCapacity[0] = 30.0;
    for (int index = 0; index < 1000; ++index)
    {
        result = StepDrivetrain(wheels, sliding, step);
    }
    Require(!result.brakeLocked[0] && sliding.rim[0] > 0.0, "the road turns a wheel past its brake");
    RequireNear(result.brakeTorque[0], -30.0, 1e-9, "a slipping brake passes its capacity");
    RequireNear(0.8 * (sliding.rim[0] + sliding.belt[0]), 20.0 * 1.0, 0.01 * 20.0, "the wheel gains the torque the brake leaves");
}

// The tyre's rolling resistance holds a wheel against a smaller push, and the brake on the rim holds
// it through the sidewall.
void TestRollingResistanceHolds()
{
    const auto wheels = Wheels(0.8, 0.8, 70000.0, 5.0);
    DrivetrainState state;
    DrivetrainStep step;
    step.beltTorque[3] = 5.0;
    step.rollingCapacity[3] = 10.0;
    DrivetrainResult result;
    for (int index = 0; index < 500; ++index)
    {
        result = StepDrivetrain(wheels, state, step);
    }
    RequireNear(state.belt[3], 0.0, 1e-12, "rolling resistance holds the belt");
    RequireNear(state.rim[3], 0.0, 1e-12, "and the rim with it");
    RequireNear(result.rollingTorque[3], -5.0, 1e-9, "against the push");
}

// At the slowest physics rate (60 Hz) the stiff parts are far past what the step resolves: it must stay
// bounded and settle, with the clutch, brakes and road torque switching.
void TestSlowStepStaysBounded()
{
    const auto wheels = Wheels(0.8, 0.8, 70000.0, 2.0);
    DrivetrainState state;
    DrivetrainStep step = RearDrive(13.0, 750.0, 350.0);
    step.dt = 1.0 / 60.0;
    double largest = 0.0;
    for (int index = 0; index < 600; ++index)
    {
        const bool braking = (index / 60) % 2 == 1;
        step.engineTorque = braking ? 0.0 : 350.0;
        step.brakeCapacity = braking ? std::array<double, 4>{1500.0, 1500.0, 1200.0, 1200.0} : std::array<double, 4>{};
        for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
        {
            // The road pushes back on the belts in proportion to their spin, like a tyre at speed.
            step.beltTorque[wheel] = -20.0 * state.belt[wheel];
        }
        StepDrivetrain(wheels, state, step);
        for (size_t wheel = 0; wheel < kDrivetrainWheels; ++wheel)
        {
            Require(std::isfinite(state.rim[wheel]) && std::isfinite(state.belt[wheel]), "speeds stay finite at 60 Hz");
            largest = std::max({largest, std::abs(state.rim[wheel]), std::abs(state.belt[wheel])});
            Require(std::abs(state.rim[wheel] - state.belt[wheel]) < 1.0, "rim and belt stay together at 60 Hz");
        }
    }
    Require(largest < 1000.0, "speeds stay bounded at 60 Hz, " + std::to_string(largest));
    std::cout << "60 Hz: fastest wheel " << largest << " rad/s\n";
}
}

int main()
{
    try
    {
        TestBeltRingsOnItsSidewall();
        TestSpinIsKept();
        TestClutchGrips();
        TestClutchSlips();
        TestBrakesHoldAndSlip();
        TestRollingResistanceHolds();
        TestSlowStepStaysBounded();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "vehicle drivetrain tests passed\n";
    return 0;
}
