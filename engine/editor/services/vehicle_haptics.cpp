#include "vehicle_haptics.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// The engine's rumble: a floor while it runs, then up with the revs, and a little more under load.
constexpr float kIdleRumble = 0.16f;
constexpr float kRevRumble = 0.34f;
constexpr float kLoadRumble = 0.14f;
// Near the limiter the small motor buzzes.
constexpr float kLimiterRevs = 0.96f;
constexpr float kLimiterBuzz = 0.3f;
// The revs settle on the rumble over about this long.
constexpr float kRevsFilterSeconds = 0.06f;

// A gear change thumps for this long, harder going up (the drive snaps in) than down.
constexpr float kShiftSeconds = 0.11f;
constexpr float kUpshiftStrength = 1.0f;
constexpr float kDownshiftStrength = 0.7f;
constexpr float kShiftLowMotor = 0.85f;
constexpr float kShiftHighMotor = 0.45f;

// The accelerator trigger is free for its first two zones, then pushes back harder towards the floor.
constexpr std::array<uint8_t, kTriggerZoneCount> kAcceleratorResistance{0, 0, 1, 1, 2, 2, 3, 3, 4, 4};
// The brake trigger starts to push back at once and ends against a wall.
constexpr std::array<uint8_t, kTriggerZoneCount> kBrakeResistance{0, 2, 3, 3, 4, 5, 5, 6, 7, 8};
constexpr float kTriggerEngagedAt = 0.3f;
// A wheel locking this far shakes the brake trigger, hard from kLockFull on; it keeps shaking this long.
constexpr float kLockStart = 0.12f;
constexpr float kLockStop = 0.07f;
constexpr float kLockFull = 0.35f;
constexpr float kLockHoldSeconds = 0.12f;
constexpr uint8_t kLockFrequencyHz = 26;
// Wheelspin this far shakes the accelerator trigger, hard from kSpinFull on.
constexpr float kSpinStart = 0.2f;
constexpr float kSpinFull = 0.6f;
constexpr uint8_t kSpinFrequencyHz = 32;
constexpr uint8_t kLimiterFrequencyHz = 40;

float Saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

// The motors move in steps of about this much, so a drifting rev count is not a stream of new reports.
float Quantize(float value)
{
    constexpr float kSteps = 48.0f;
    return std::round(Saturate(value) * kSteps) / kSteps;
}

uint8_t ScaledStrength(uint8_t base, float scale)
{
    if (base == 0 || scale <= 0.0f)
    {
        return 0;
    }
    const float scaled = std::round(static_cast<float>(base) * scale);
    return static_cast<uint8_t>(std::clamp(scaled, 1.0f, static_cast<float>(kTriggerMaxStrength)));
}

TriggerEffect Resistance(const std::array<uint8_t, kTriggerZoneCount>& strengths, float scale)
{
    TriggerEffect effect;
    for (size_t zone = 0; zone < kTriggerZoneCount; ++zone)
    {
        effect.zoneStrength[zone] = ScaledStrength(strengths[zone], scale);
        if (effect.zoneStrength[zone] != 0)
        {
            effect.mode = TriggerEffect::Mode::Resistance;
        }
    }
    return effect;
}

// A shake from `fromZone` to the floor; `amount` (0 to 1) is how hard, over amplitudes 3 to 8.
TriggerEffect Vibration(size_t fromZone, float amount, uint8_t frequencyHz, float scale)
{
    TriggerEffect effect;
    const uint8_t amplitude = ScaledStrength(static_cast<uint8_t>(std::lround(3.0f + 5.0f * Saturate(amount))), scale);
    if (amplitude == 0)
    {
        return effect;
    }
    effect.mode = TriggerEffect::Mode::Vibration;
    effect.frequencyHz = frequencyHz;
    for (size_t zone = fromZone; zone < kTriggerZoneCount; ++zone)
    {
        effect.zoneStrength[zone] = amplitude;
    }
    return effect;
}
}

GamepadFeedback ComputeVehicleFeedback(const VehicleHapticsSettings& settings, const VehicleHapticsInput& input, VehicleHapticsState& state, float deltaSeconds)
{
    const VehicleTelemetry& telemetry = input.telemetry;
    const float dt = std::max(deltaSeconds, 0.0f);

    // A gear change between forward gears is felt; moving off from neutral or flipping to reverse is not.
    if (state.hasGear && telemetry.gear != state.lastGear && telemetry.gear >= 1 && state.lastGear >= 1)
    {
        state.shiftStrength = telemetry.gear > state.lastGear ? kUpshiftStrength : kDownshiftStrength;
        state.shiftSeconds = kShiftSeconds;
        state.shiftAge = 0.0f;
    }
    state.hasGear = true;
    state.lastGear = telemetry.gear;
    state.shiftAge += dt;

    const float revRange = std::max(input.maxRpm - input.minRpm, 1.0f);
    const float revs = Saturate((telemetry.engineRpm - input.minRpm) / revRange);
    const float blend = dt > 0.0f ? 1.0f - std::exp(-dt / kRevsFilterSeconds) : 1.0f;
    state.smoothedRevs += (revs - state.smoothedRevs) * blend;

    GamepadFeedback feedback;
    if (!settings.enabled)
    {
        return feedback;
    }

    // The thump falls away over its length.
    float shift = 0.0f;
    if (state.shiftSeconds > 0.0f && state.shiftAge < state.shiftSeconds)
    {
        shift = state.shiftStrength * (1.0f - state.shiftAge / state.shiftSeconds);
    }

    const float rumbleScale = std::max(settings.rumbleStrength, 0.0f);
    const bool engineRunning = telemetry.engineRpm > 1.0f;
    float low = 0.0f;
    float high = 0.0f;
    if (engineRunning)
    {
        low = kIdleRumble + kRevRumble * state.smoothedRevs + kLoadRumble * Saturate(input.rightTrigger);
        if (revs >= kLimiterRevs)
        {
            high = kLimiterBuzz;
        }
    }
    low = std::max(low, shift * kShiftLowMotor);
    high = std::max(high, shift * kShiftHighMotor);
    feedback.lowFrequencyMotor = Quantize(low * rumbleScale);
    feedback.highFrequencyMotor = Quantize(high * rumbleScale);

    if (!input.adaptiveTriggers)
    {
        return feedback;
    }

    const float triggerScale = std::max(settings.triggerStrength, 0.0f);

    // The brake: a locking wheel shakes it, and it stays shaking a moment so the pulse is felt.
    const bool braking = input.leftTrigger > kTriggerEngagedAt;
    const bool locking = settings.brakeLockFeedback && braking && telemetry.lockSlip > (state.brakeLockHold > 0.0f ? kLockStop : kLockStart);
    if (locking)
    {
        state.brakeLockHold = kLockHoldSeconds;
    }
    else
    {
        state.brakeLockHold = std::max(state.brakeLockHold - dt, 0.0f);
    }
    if (braking && state.brakeLockHold > 0.0f)
    {
        const float hardness = (telemetry.lockSlip - kLockStart) / (kLockFull - kLockStart);
        feedback.leftTrigger = Vibration(2, hardness, kLockFrequencyHz, triggerScale);
    }
    else
    {
        feedback.leftTrigger = Resistance(kBrakeResistance, triggerScale);
    }

    // The accelerator: wheelspin and the rev limiter shake it, otherwise it pushes back like a pedal.
    const bool pushing = input.rightTrigger > kTriggerEngagedAt;
    if (pushing && telemetry.spinSlip > kSpinStart)
    {
        feedback.rightTrigger = Vibration(1, (telemetry.spinSlip - kSpinStart) / (kSpinFull - kSpinStart), kSpinFrequencyHz, triggerScale);
    }
    else if (pushing && revs >= kLimiterRevs)
    {
        feedback.rightTrigger = Vibration(2, 0.4f, kLimiterFrequencyHz, triggerScale);
    }
    else
    {
        feedback.rightTrigger = Resistance(kAcceleratorResistance, triggerScale);
    }
    return feedback;
}
}
