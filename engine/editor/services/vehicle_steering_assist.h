#pragma once

namespace me
{

// How a gamepad's stick (or the keyboard) steers the car, as Gran Turismo 7 does for a controller: the
// stick asks for a share of the lock the car can use at its speed rather than of the rack's full lock,
// the front wheels follow it at a limited rate, and that range is centred on where the car is going, so
// a sliding car is caught with the stick held straight.
struct VehicleSteeringAssistSettings
{
    bool enabled = true;
    // The stick's response: 0 linear, 1 cubic (fine near the centre, quick near the edge).
    float sensitivity = 0.5f;
    // The fastest the steering sweeps from centre to the stick's full deflection, and back towards the centre.
    float steerSeconds = 0.25f;
    float returnSeconds = 0.15f;
    // A short smoothing on top of the rate limit, so a flick does not jerk the rack.
    float smoothingSeconds = 0.04f;
    // Full deflection at speed turns the front wheels to the angle the car needs for a corner taken at
    // `cornerGrip` g, plus the front tyres' peak slip angle: past that they only scrub.
    bool speedSensitive = true;
    float cornerGrip = 1.0f;
    // Below this speed (m/s) the full lock is always there, for parking and hairpins.
    float fullLockSpeed = 5.0f;
    // The least share of the full lock the stick can ask for at any speed.
    float minLockShare = 0.1f;
    // The range is centred on the car's travel when it slides more than `counterSteerDeadZoneDegrees`
    // (the body's slip angle): let go and the wheels point where the car is going.
    bool counterSteerAssist = true;
    float counterSteerDeadZoneDegrees = 2.0f;
};

// The car the steering is for, this frame.
struct VehicleSteeringAssistInput
{
    // What the driver asks for, -1 full left to 1 full right.
    float request = 0.0f;
    // The body's velocity along its forward axis and to its right (m/s).
    float forwardSpeed = 0.0f;
    float rightSpeed = 0.0f;
    // The front wheels' angle at full lock (degrees), the wheelbase (m), and the front tyres' peak slip
    // angle (degrees).
    float maxSteerDegrees = 35.0f;
    float wheelbase = 2.6f;
    float peakSlipDegrees = 7.0f;
};

// Carried from frame to frame.
struct VehicleSteeringAssistState
{
    // The driver's request after the response curve, rate limit and smoothing (-1 to 1).
    float request = 0.0f;
    // Its rate-limited value before the smoothing.
    float limited = 0.0f;
    // The share of the full lock the centre of the range sits at, smoothed.
    float centre = 0.0f;
};

// What the stick's response curve makes of `request` (-1 to 1).
float ApplySteeringResponse(float request, float sensitivity);

// The share of the full lock that full deflection gives at `forwardSpeed` (m/s), 0 to 1.
float ComputeSteeringLockShare(const VehicleSteeringAssistSettings& settings, const VehicleSteeringAssistInput& input);

// The steering for this frame, -1 full left to 1 full right of the rack's full lock. Disabled, it is the
// request as it is.
float ComputeAssistedSteering(
    const VehicleSteeringAssistSettings& settings, const VehicleSteeringAssistInput& input, VehicleSteeringAssistState& state, float deltaSeconds);
}
