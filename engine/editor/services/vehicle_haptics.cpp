#include "vehicle_haptics.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// The engine's rumble: a floor while it runs, then up with the revs, and a little more under load.
constexpr float kIdleRumble = 0.05f;
constexpr float kRevRumble = 0.10f;
constexpr float kLoadRumble = 0.04f;
// Near the limiter the small motor buzzes.
constexpr float kLimiterRevs = 0.96f;
constexpr float kLimiterBuzz = 0.12f;
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
// The clutch carries the drive from here up (its friction, 0 to 1): below it the accelerator trigger
// hangs slack, and when it climbs back past it the trigger shoves at the finger, as hard as
// kBiteShove plus kBiteLoadShove under full throttle; the shove holds kBiteHoldSeconds, then fades back
// to the pedal's own resistance over kBiteFadeSeconds.
constexpr float kBiteClutch = 0.6f;
constexpr float kBiteShove = 0.6f;
constexpr float kBiteLoadShove = 0.4f;
constexpr float kBiteHoldSeconds = 0.06f;
constexpr float kBiteFadeSeconds = 0.18f;

// The actuators. The engine's beat: a floor while it runs, then up with the revs and the throttle.
constexpr float kEngineIdleLevel = 0.05f;
constexpr float kEngineRevLevel = 0.10f;
constexpr float kEngineLoadLevel = 0.05f;
constexpr float kLimiterThrottle = 0.3f;
// The road: a faint grain from the tarmac that grows with speed up to kGrainFullSpeed, and more as the
// suspension works over rough ground (an RMS of kRoughnessFull in m/s is as rough as it gets).
constexpr float kRoadGrainLevel = 0.10f;
constexpr float kGrainFullSpeed = 30.0f;
constexpr float kRoadRoughLevel = 0.60f;
constexpr float kRoughnessFull = 0.25f;
// The suspension's motion is split at these time constants: slower is the car rolling and pitching,
// and the roughness is the RMS of what is left over about this long.
constexpr float kSlowVelocitySeconds = 0.25f;
constexpr float kRoughnessSeconds = 0.15f;
// A bump: the suspension jumping this fast (m/s) and well clear of the road's usual roughness knocks
// the hand on that side, as hard as kBumpFull gives at most; one knock per wheel in kBumpCooldown.
constexpr float kBumpVelocity = 0.25f;
constexpr float kBumpOverRoughness = 2.5f;
constexpr float kBumpFull = 1.0f;
constexpr float kBumpLevel = 0.9f;
constexpr float kBumpCooldownSeconds = 0.08f;
// A sliding tyre: how far it slides (the brush tyre's sliding share of the load, or the slip itself),
// faded in over the first kSlideFullSpeed m/s of sliding speed, its buzz rising with that speed.
constexpr float kSlidingShareStart = 0.15f;
constexpr float kSlidingShareFull = 0.75f;
constexpr float kSlipRatioStart = 0.08f;
constexpr float kSlipRatioFull = 0.33f;
constexpr float kSlipAngleStartDegrees = 5.0f;
constexpr float kSlipAngleFullDegrees = 15.0f;
constexpr float kSlideFullSpeed = 3.0f;
constexpr float kSlipLevel = 0.7f;
constexpr float kSlipBaseHz = 70.0f;
constexpr float kSlipHzPerMetrePerSecond = 10.0f;
constexpr float kSlipMaxHz = 220.0f;
// A gear change knocks both hands.
constexpr float kUpshiftKick = 0.6f;
constexpr float kDownshiftKick = 0.45f;
// And once the clutch bites past kBiteClutch the drive comes back with a shunt, harder under throttle.
constexpr float kBiteKick = 0.3f;
constexpr float kBiteLoadKick = 0.5f;

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

// The accelerator trigger as the clutch bites: the whole travel stiff at `amount` (0 to 1) of the most
// it can push, never softer than the pedal's own resistance.
TriggerEffect Shove(float amount, float scale)
{
    std::array<uint8_t, kTriggerZoneCount> strengths{};
    const uint8_t shove = static_cast<uint8_t>(std::lround(static_cast<float>(kTriggerMaxStrength) * std::clamp(amount, 0.0f, 1.0f)));
    for (size_t zone = 0; zone < kTriggerZoneCount; ++zone)
    {
        strengths[zone] = std::max(shove, kAcceleratorResistance[zone]);
    }
    return Resistance(strengths, scale);
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

float Blend(float deltaSeconds, float timeConstantSeconds)
{
    return deltaSeconds > 0.0f ? 1.0f - std::exp(-deltaSeconds / timeConstantSeconds) : 0.0f;
}

// How far a wheel's tyre is sliding, from 0 (gripping) to 1.
float Sliding(const VehicleWheelState& wheel)
{
    if (wheel.brushTyre)
    {
        return Saturate((wheel.slidingShare - kSlidingShareStart) / (kSlidingShareFull - kSlidingShareStart));
    }
    const float ratio = (std::abs(wheel.slipRatio) - kSlipRatioStart) / (kSlipRatioFull - kSlipRatioStart);
    const float angle = (std::abs(wheel.slipAngleDegrees) - kSlipAngleStartDegrees) / (kSlipAngleFullDegrees - kSlipAngleStartDegrees);
    return Saturate(std::max(ratio, angle));
}

// How fast the tread rubs over the ground (m/s): the wheel spinning against the road, and the road
// running across it.
float SlidingSpeed(const VehicleWheelState& wheel, const VehicleTelemetry& telemetry)
{
    const float alongSlide = wheel.angularVelocity * wheel.radius - telemetry.forwardSpeed;
    const float speed = std::hypot(telemetry.forwardSpeed, telemetry.rightSpeed);
    const float acrossSlide = speed * std::sin(glm::radians(std::min(std::abs(wheel.slipAngleDegrees), 90.0f)));
    return std::hypot(alongSlide, acrossSlide);
}
}

VehicleAudioHaptics ComputeVehicleAudioHaptics(const VehicleHapticsSettings& settings, const VehicleAudioHapticsInput& input, VehicleAudioHapticsState& state,
                                               float deltaSeconds)
{
    const VehicleTelemetry& telemetry = input.telemetry;
    const float dt = std::max(deltaSeconds, 0.0f);
    VehicleAudioHaptics haptics;

    const float revRange = std::max(input.maxRpm - input.minRpm, 1.0f);
    const float revs = Saturate((telemetry.engineRpm - input.minRpm) / revRange);
    const float throttle = Saturate(input.rightTrigger);

    // A gear change between forward gears, as the rumble has it, and the drive coming back once the
    // clutch bites in the new gear (a frame later at the soonest, for a box quicker than a frame). With
    // the adaptive triggers on, the accelerator trigger going slack is the change starting, not a knock.
    const bool triggersCarryShift = settings.triggerStrength > 0.0f;
    float shiftKick = 0.0f;
    if (state.awaitingBite && telemetry.clutch >= kBiteClutch && telemetry.gear >= 1)
    {
        shiftKick = kBiteKick + kBiteLoadKick * throttle;
        state.awaitingBite = false;
    }
    if (state.hasGear && telemetry.gear != state.lastGear && telemetry.gear >= 1 && state.lastGear >= 1)
    {
        if (!triggersCarryShift)
        {
            shiftKick = telemetry.gear > state.lastGear ? kUpshiftKick : kDownshiftKick;
        }
        state.awaitingBite = true;
    }
    state.hasGear = true;
    state.lastGear = telemetry.gear;
    HapticsVoices& voices = haptics.voices;
    if (telemetry.engineRpm > 1.0f)
    {
        voices.engineHz = telemetry.engineRpm / 60.0f * static_cast<float>(std::max(input.cylinders, 1)) * 0.5f;
        voices.engineAmplitude = kEngineIdleLevel + kEngineRevLevel * revs + kEngineLoadLevel * throttle;
        voices.limiter = revs >= kLimiterRevs && throttle > kLimiterThrottle;
    }

    const float speed = std::hypot(telemetry.forwardSpeed, telemetry.rightSpeed);
    voices.roadSpeed = speed;
    const float grain = kRoadGrainLevel * Saturate(speed / kGrainFullSpeed);
    const glm::vec3 carRight = input.body.rotation * glm::vec3(-1.0f, 0.0f, 0.0f);
    const float slowBlend = Blend(dt, kSlowVelocitySeconds);
    const float roughnessBlend = Blend(dt, kRoughnessSeconds);
    float fastestSlide = 0.0f;

    state.wheels.resize(input.wheels.size());
    for (size_t index = 0; index < input.wheels.size(); ++index)
    {
        const VehicleWheelState& wheel = input.wheels[index];
        VehicleAudioHapticsState::Wheel& track = state.wheels[index];
        const size_t side = glm::dot(glm::vec3(wheel.pose.position - input.body.position), carRight) > 0.0f ? 1 : 0;

        // The suspension's and the tyre's compression together: what the road pushes up at the hub.
        const float compression = wheel.suspensionMaxLength - wheel.suspensionLength + (wheel.unsprungMass ? wheel.tyreDeflection : 0.0f);
        track.kickCooldown = std::max(track.kickCooldown - dt, 0.0f);
        if (track.known && dt > 0.0f)
        {
            const float velocity = (compression - track.compression) / dt;
            track.slowVelocity += (velocity - track.slowVelocity) * slowBlend;
            const float jolt = std::abs(velocity - track.slowVelocity);
            if (wheel.inContact && jolt > kBumpVelocity && jolt > kBumpOverRoughness * track.roughness && track.kickCooldown <= 0.0f)
            {
                haptics.kicks[side] = std::max(haptics.kicks[side], kBumpLevel * Saturate(jolt / kBumpFull));
                track.kickCooldown = kBumpCooldownSeconds;
            }
            const float meanSquare = track.roughness * track.roughness;
            track.roughness = std::sqrt(meanSquare + (jolt * jolt - meanSquare) * roughnessBlend);
        }
        track.known = true;
        track.compression = compression;

        if (!wheel.inContact)
        {
            continue;
        }
        const float road = grain + kRoadRoughLevel * Saturate(track.roughness / kRoughnessFull);
        voices.roadAmplitude[side] = std::max(voices.roadAmplitude[side], road);

        const float slidingSpeed = SlidingSpeed(wheel, telemetry);
        const float slip = kSlipLevel * Sliding(wheel) * Saturate(slidingSpeed / kSlideFullSpeed);
        if (slip > 0.0f)
        {
            voices.slipAmplitude[side] = std::max(voices.slipAmplitude[side], slip);
            fastestSlide = std::max(fastestSlide, slidingSpeed);
        }
    }
    voices.slipHz = std::min(kSlipBaseHz + kSlipHzPerMetrePerSecond * fastestSlide, kSlipMaxHz);

    if (!settings.enabled || !settings.audioHaptics)
    {
        return VehicleAudioHaptics{};
    }

    const float engineScale = std::max(settings.engineStrength, 0.0f);
    const float roadScale = std::max(settings.roadStrength, 0.0f);
    const float slipScale = std::max(settings.slipStrength, 0.0f);
    voices.engineAmplitude *= engineScale;
    for (size_t side = 0; side < kHapticsSides; ++side)
    {
        voices.roadAmplitude[side] *= roadScale;
        voices.slipAmplitude[side] *= slipScale;
        haptics.kicks[side] = Saturate(std::max(haptics.kicks[side] * roadScale, shiftKick * engineScale));
    }
    return haptics;
}

GamepadFeedback ComputeVehicleFeedback(const VehicleHapticsSettings& settings, const VehicleHapticsInput& input, VehicleHapticsState& state, float deltaSeconds)
{
    const VehicleTelemetry& telemetry = input.telemetry;
    const float dt = std::max(deltaSeconds, 0.0f);

    // A gear change between forward gears is felt; moving off from neutral or flipping to reverse is not.
    const bool shifted = state.hasGear && telemetry.gear != state.lastGear && telemetry.gear >= 1 && state.lastGear >= 1;
    if (shifted)
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
    // With adaptive triggers the accelerator carries the gear change instead (below).
    const float triggerScale = std::max(settings.triggerStrength, 0.0f);
    if (!input.adaptiveTriggers || triggerScale <= 0.0f)
    {
        low = std::max(low, shift * kShiftLowMotor);
        high = std::max(high, shift * kShiftHighMotor);
    }
    feedback.lowFrequencyMotor = Quantize(low * rumbleScale);
    feedback.highFrequencyMotor = Quantize(high * rumbleScale);

    // The drive through the clutch: cut while it is open, and coming back with a shove as it bites (at
    // once, for a box that changes quicker than a frame and never shows the clutch open).
    const bool driveCut = telemetry.clutch < kBiteClutch;
    if ((state.driveCut || shifted) && !driveCut)
    {
        state.biteStrength = kBiteShove + kBiteLoadShove * Saturate(input.rightTrigger);
        state.biteAge = 0.0f;
    }
    else
    {
        state.biteAge = std::min(state.biteAge + dt, 1.0f);
    }
    state.driveCut = driveCut;

    if (!input.adaptiveTriggers)
    {
        return feedback;
    }

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

    // The accelerator: slack while the clutch is open, shoving back as it bites; then wheelspin and the
    // rev limiter shake it, otherwise it pushes back like a pedal.
    const bool pushing = input.rightTrigger > kTriggerEngagedAt;
    const float biteLeft = state.biteAge < kBiteHoldSeconds ? 1.0f : 1.0f - (state.biteAge - kBiteHoldSeconds) / kBiteFadeSeconds;
    if (driveCut)
    {
        feedback.rightTrigger = TriggerEffect{};
    }
    else if (biteLeft > 0.0f)
    {
        feedback.rightTrigger = Shove(state.biteStrength * biteLeft, triggerScale);
    }
    else if (pushing && telemetry.spinSlip > kSpinStart)
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
