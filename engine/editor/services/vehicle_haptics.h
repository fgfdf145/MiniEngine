#pragma once

#include <engine/audio/haptics_synth.h>
#include <engine/core/input/gamepad_feedback.h>
#include <engine/physics/physics_world.h>

#include <vector>

namespace me
{

// What the gamepad does to the driver's hands while a car is driven.
struct VehicleHapticsSettings
{
    bool enabled = true;
    // The engine's low rumble and the gearbox's thump: a scale on the motors (0 is off, the default).
    float rumbleStrength = 0.0f;
    // The adaptive triggers' resistance and vibration (a DualSense): a scale on their strengths (0 is off).
    // While they are on they carry the gear change: the accelerator goes slack while the clutch is open
    // and shoves back at the finger as it bites, in place of the motors' and actuators' thump.
    float triggerStrength = 1.0f;
    // The brake trigger shakes while a wheel locks, as an ABS pulses the pedal.
    bool brakeLockFeedback = true;
    // A DualSense on USB: its actuators play the engine, the road and the tyres as waveforms
    // (GamepadHaptics) in place of the rumble motors. Each voice has its own scale (0 is off).
    bool audioHaptics = true;
    float engineStrength = 1.0f;
    float roadStrength = 1.0f;
    float slipStrength = 1.0f;
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
    // The clutch is open (the accelerator trigger hangs slack), and the shove when it bites again: how
    // hard (0 to 1) and how long since.
    bool driveCut = false;
    float biteStrength = 0.0f;
    float biteAge = 1.0f;
};

// What the actuators' waveforms are made from: the car's wheels and where its body is, besides
// what ComputeVehicleFeedback takes.
struct VehicleAudioHapticsInput
{
    VehicleTelemetry telemetry;
    std::vector<VehicleWheelState> wheels;
    PhysicsPose body;
    float minRpm = 1000.0f;
    float maxRpm = 7000.0f;
    // Cylinders fire once each every two turns of the crank: the engine's beat is rpm / 60 * cylinders / 2.
    int cylinders = 6;
    float rightTrigger = 0.0f;
};

// Carried from frame to frame: each wheel's suspension, to find the road's bumps in its motion.
struct VehicleAudioHapticsState
{
    struct Wheel
    {
        bool known = false;
        // The suspension's and tyre's combined compression (m) last frame, how fast it moved (m/s)
        // averaged over a while (the car rolling and pitching, which the road's texture is not), and the
        // RMS of the rest: how rough the road is under the wheel.
        float compression = 0.0f;
        float slowVelocity = 0.0f;
        float roughness = 0.0f;
        // A bump only knocks again once this has run out.
        float kickCooldown = 0.0f;
    };
    std::vector<Wheel> wheels;
    bool hasGear = false;
    int lastGear = 0;
    // A gear change opened the clutch: when it bites again the drive comes back with a shunt.
    bool awaitingBite = false;
};

// One frame of the actuators' waveforms: the levels, and the thumps (0 to 1) to start on each side.
struct VehicleAudioHaptics
{
    HapticsVoices voices;
    std::array<float, kHapticsSides> kicks{};
};

// The actuators for one frame of driving: the engine's firing beat (and the limiter chopping it), the
// road's grain under each side's wheels with a knock at each bump, the tyres' buzz as they slide, and a
// shunt as the clutch bites after a gear change (and, with the adaptive triggers off, a thump as the
// change starts). Silent when settings turn them off.
VehicleAudioHaptics ComputeVehicleAudioHaptics(const VehicleHapticsSettings& settings, const VehicleAudioHapticsInput& input, VehicleAudioHapticsState& state,
                                               float deltaSeconds);

// The rumble and trigger effects for one frame of driving: an engine rumble that grows with the revs
// and the throttle, an accelerator trigger that pushes back (and shakes at the rev limiter or in
// wheelspin), and a brake trigger that is stiffer (and shakes while a wheel locks). A gear change is felt
// in the accelerator trigger, which goes slack while the clutch is open and shoves back as it bites; a
// pad without adaptive triggers thumps its motors instead.
GamepadFeedback ComputeVehicleFeedback(const VehicleHapticsSettings& settings, const VehicleHapticsInput& input, VehicleHapticsState& state, float deltaSeconds);
}
