#pragma once

#include <array>
#include <cstddef>

namespace me
{

// The car's turning parts, stepped together: the engine, and each wheel as two bodies, the rim (with
// the hub, the brake disc and the half-shaft's end) and the tyre's belt (its tread and the belts under
// it), joined by the sidewalls' twist. The clutch couples the engine to the rims through the gearbox
// and the differentials; the brakes hold the rims; the road and the tyre's rolling resistance act on
// the belts. See docs/design/2026-10-10-own-drivetrain-design.md.
//
// The clutch, the brakes and the rolling resistance are dry friction: each holds its two sides
// together up to its capacity and slips at the capacity beyond it. The step is linearly implicit (the
// sidewalls' spring by the trapezoidal rule where the step resolves its ring, their damping and the engine's drag backward), with the
// frictions solved as a box-constrained complementarity problem over the step's impulses.

constexpr size_t kDrivetrainWheels = 4;

struct DrivetrainWheel
{
    double rimInertia = 1.0;            // kg m^2
    double beltInertia = 0.5;           // kg m^2
    double sidewallStiffness = 50000.0; // N m/rad, the belt's twist against the rim
    double sidewallDamping = 5.0;       // N m s/rad
};

struct DrivetrainState
{
    double engine = 0.0; // rad/s
    std::array<double, kDrivetrainWheels> rim{};
    std::array<double, kDrivetrainWheels> belt{};
    // How far each rim has turned ahead of its belt (rad): the sidewalls' twist.
    std::array<double, kDrivetrainWheels> twist{};
};

// What acts on the parts over one step. Torques are about the axles, positive turning forward (the
// engine its own way); capacities are the most a friction holds (N m, 0 for none).
struct DrivetrainStep
{
    double dt = 0.001;
    double engineInertia = 0.2;
    double engineDamping = 0.0; // 1/s: a drag of engineDamping * engineInertia * speed
    double engineTorque = 0.0;
    // The clutch: each rim's weight in its far side's speed, engine speed = sum(weight * rim speed) once
    // it grips, and the share of the clutch's torque each rim takes times the gearing to it (the
    // overall ratio times the wheel's share of the drive, signed in reverse).
    std::array<double, kDrivetrainWheels> clutchWeights{};
    double clutchCapacity = 0.0;
    std::array<double, kDrivetrainWheels> rimTorque{};
    std::array<double, kDrivetrainWheels> beltTorque{};
    // How the road's torque on each belt falls as the belt gains speed over the step (N m s/rad): the tyre
    // linearised about beltTorque, so its stiff grip is stepped implicitly with the rest.
    std::array<double, kDrivetrainWheels> beltDamping{};
    std::array<double, kDrivetrainWheels> brakeCapacity{};
    std::array<double, kDrivetrainWheels> rollingCapacity{};
};

struct DrivetrainResult
{
    double clutchTorque = 0.0; // what the clutch passed from the engine to the wheels, N m at the engine
    bool clutchLocked = false;
    std::array<double, kDrivetrainWheels> brakeTorque{};    // on each rim, N m
    std::array<double, kDrivetrainWheels> rollingTorque{};  // on each belt, N m
    std::array<double, kDrivetrainWheels> sidewallTorque{}; // from each rim to its belt, the step's mean
    std::array<bool, kDrivetrainWheels> brakeLocked{};
    int pivots = 0;
    bool converged = true;
};

// One step of dt: `state` moves to the step's end.
DrivetrainResult StepDrivetrain(const std::array<DrivetrainWheel, kDrivetrainWheels>& wheels, DrivetrainState& state, const DrivetrainStep& step);
}
