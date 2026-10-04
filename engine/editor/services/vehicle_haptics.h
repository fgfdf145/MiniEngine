#pragma once

#include <engine/core/input/gamepad_feedback.h>
#include <engine/physics/physics_world.h>

namespace me
{

// What the gamepad does to the driver's hands while a car is driven.
struct VehicleHapticsSettings
{
    bool enabled = true;
    // The engine's low rumble and the gearbox's thump: a scale on the motors (0 is off).
    float rumbleStrength = 1.0f;
    // The adaptive triggers' resistance and vibration (a DualSense): a scale on their strengths (0 is off).
    float triggerStrength = 1.0f;
    // The brake trigger shakes while a wheel locks, as an ABS pulses the pedal.
    bool brakeLockFeedback = true;
};

// What the feedback is made from this frame.
struct VehicleHapticsInput
{
    VehicleTelemetry telemetry;
    // The engine's idle and rev limit (rpm), which its revs are placed between.
    float minRpm = 1000.0f;
    float maxRpm = 7000.0f;
    // How far the gamepad's right and left triggers are pulled (0 to 1): the accelerator and the brake.
    float rightTrigger = 0.0f;
    float leftTrigger = 0.0f;
    // A DualSense: its triggers can push back and shake. Other pads get the motors alone.
    bool adaptiveTriggers = false;
};

// Carried from frame to frame.
struct VehicleHapticsState
{
    bool hasGear = false;
    int lastGear = 0;
    // The gear change's thump: its strength at the start, how long it lasts, and how long since.
    float shiftStrength = 0.0f;
    float shiftSeconds = 0.0f;
    float shiftAge = 1.0f;
    // The engine's revs between idle (0) and the limit (1), smoothed so the rumble follows them without steps.
    float smoothedRevs = 0.0f;
    // How long the brake trigger keeps shaking after a wheel stopped locking, so it does not flicker.
    float brakeLockHold = 0.0f;
};

// The rumble and trigger effects for one frame of driving: an engine rumble that grows with the revs
// and the throttle, a thump at each gear change, an accelerator trigger that pushes back (and shakes at
// the rev limiter or in wheelspin), and a brake trigger that is stiffer (and shakes while a wheel locks).
GamepadFeedback ComputeVehicleFeedback(const VehicleHapticsSettings& settings, const VehicleHapticsInput& input, VehicleHapticsState& state, float deltaSeconds);
}
