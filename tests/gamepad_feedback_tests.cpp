#include <engine/core/input/gamepad_feedback.h>
#include <engine/editor/services/vehicle_haptics.h>

#include <iostream>
#include <stdexcept>
#include <string>

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

// The report's layout is the controller's, so every byte it needs is pinned.
void EncodesTheEffectsState()
{
    GamepadFeedback feedback;
    feedback.lowFrequencyMotor = 1.0f;
    feedback.highFrequencyMotor = 0.5f;
    feedback.rightTrigger.mode = TriggerEffect::Mode::Resistance;
    feedback.rightTrigger.zoneStrength[2] = 1;
    feedback.rightTrigger.zoneStrength[9] = 8;
    feedback.leftTrigger.mode = TriggerEffect::Mode::Vibration;
    feedback.leftTrigger.zoneStrength[0] = 3;
    feedback.leftTrigger.frequencyHz = 30;

    const std::array<uint8_t, kDualSenseEffectsSize> state = EncodeDualSenseEffects(feedback);
    Require(state[0] == 0x0F, "enable bits claim rumble, audio haptics off and both triggers");
    Require(state[3] == 255, "the left motor is the low frequency one at full");
    Require(state[2] == 128, "the right motor is the high frequency one at half");

    // Right trigger at byte 10: zones 2 and 9, strengths 0 and 7 (less one) in three bits each.
    Require(state[10] == 0x21, "the right trigger is in the multi-position feedback mode");
    Require(state[11] == ((1 << 2) | 0) && state[12] == (1 << 1), "zones 2 and 9 are active");
    const uint32_t strengths = state[13] | (state[14] << 8) | (state[15] << 16) | (static_cast<uint32_t>(state[16]) << 24);
    Require(strengths == (7u << 27), "zone 9 holds strength 8 and zone 2 holds strength 1");

    // Left trigger at byte 21: vibration with its frequency at the tenth byte.
    Require(state[21] == 0x26, "the left trigger is in the multi-position vibration mode");
    Require(state[22] == 1 && state[23] == 0, "only zone 0 is active");
    Require((state[24] & 0x07) == 2, "zone 0 holds amplitude 3");
    Require(state[30] == 30, "the frequency follows the zone data");
}

void IdleFreesEverything()
{
    const GamepadFeedback idle;
    Require(idle.IsIdle(), "a default feedback is idle");
    const std::array<uint8_t, kDualSenseEffectsSize> state = EncodeDualSenseEffects(idle);
    Require(state[2] == 0 && state[3] == 0, "motors are off");
    Require(state[10] == 0 && state[21] == 0, "triggers are free");
}

void AudioHapticsLeaveTheRumbleEmulationOff()
{
    GamepadFeedback feedback;
    feedback.audioHaptics = true;
    feedback.lowFrequencyMotor = 1.0f;
    feedback.rightTrigger.mode = TriggerEffect::Mode::Resistance;
    feedback.rightTrigger.zoneStrength[4] = 2;
    const std::array<uint8_t, kDualSenseEffectsSize> state = EncodeDualSenseEffects(feedback);
    Require(state[0] == 0x0C, "only the triggers are claimed, which hands the actuators to the audio");
    Require(state[2] == 0 && state[3] == 0, "the motors are not used");
    Require(state[10] == 0x21, "the triggers still work");
    Require(!GamepadFeedback{.audioHaptics = true}.IsIdle(), "audio haptics alone are still sent to the pad");
}

// Four wheels on a car facing +Z: its right is -X.
VehicleAudioHapticsInput Rolling(float speed, float rpm)
{
    VehicleAudioHapticsInput input;
    input.telemetry.forwardSpeed = speed;
    input.telemetry.engineRpm = rpm;
    input.telemetry.gear = 3;
    for (const glm::vec3 position : {glm::vec3(0.8f, 0.3f, 1.3f), glm::vec3(-0.8f, 0.3f, 1.3f), glm::vec3(0.8f, 0.3f, -1.3f), glm::vec3(-0.8f, 0.3f, -1.3f)})
    {
        VehicleWheelState wheel;
        wheel.pose.position = position;
        wheel.inContact = true;
        wheel.radius = 0.33f;
        wheel.angularVelocity = speed / wheel.radius;
        wheel.suspensionMaxLength = 0.3f;
        wheel.suspensionLength = 0.2f;
        input.wheels.push_back(wheel);
    }
    return input;
}

void AudioHapticsFollowTheCar()
{
    VehicleHapticsSettings settings;
    VehicleAudioHapticsState state;
    constexpr float kDt = 1.0f / 60.0f;

    VehicleAudioHaptics haptics;
    for (int frame = 0; frame < 30; ++frame)
    {
        haptics = ComputeVehicleAudioHaptics(settings, Rolling(20.0f, 3000.0f), state, kDt);
    }
    Require(std::abs(haptics.voices.engineHz - 150.0f) < 0.01f, "a six at 3000 rpm fires 150 times a second");
    Require(haptics.voices.engineAmplitude > 0.0f && !haptics.voices.limiter, "the engine beats");
    Require(haptics.voices.roadAmplitude[0] > 0.0f && haptics.voices.roadAmplitude[0] == haptics.voices.roadAmplitude[1], "a smooth road has its grain on both sides");
    Require(haptics.kicks[0] == 0.0f && haptics.kicks[1] == 0.0f, "a smooth road does not knock");
    Require(haptics.voices.slipAmplitude[0] == 0.0f && haptics.voices.slipAmplitude[1] == 0.0f, "gripping tyres are still");

    // The front right wheel (x < 0) hits a bump: its suspension compresses 3 cm in a frame.
    VehicleAudioHapticsInput bump = Rolling(20.0f, 3000.0f);
    bump.wheels[1].suspensionLength -= 0.03f;
    haptics = ComputeVehicleAudioHaptics(settings, bump, state, kDt);
    Require(haptics.kicks[1] > 0.5f && haptics.kicks[0] == 0.0f, "the bump knocks the right hand");
    haptics = ComputeVehicleAudioHaptics(settings, bump, state, kDt);
    Require(haptics.kicks[1] == 0.0f, "one knock per bump");
    Require(haptics.voices.roadAmplitude[1] > haptics.voices.roadAmplitude[0], "the right side feels the rougher road a while");

    // The left rear wheel slides sideways.
    VehicleAudioHapticsInput slide = Rolling(20.0f, 3000.0f);
    slide.wheels[2].suspensionLength -= 0.03f; // where the right one is now: no new bump
    slide.wheels[1].suspensionLength -= 0.03f;
    slide.wheels[0].suspensionLength -= 0.03f;
    slide.wheels[3].suspensionLength -= 0.03f;
    VehicleAudioHapticsState slideState;
    ComputeVehicleAudioHaptics(settings, slide, slideState, kDt);
    slide.wheels[2].slipAngleDegrees = 14.0f;
    haptics = ComputeVehicleAudioHaptics(settings, slide, slideState, kDt);
    Require(haptics.voices.slipAmplitude[0] > 0.3f && haptics.voices.slipAmplitude[1] == 0.0f, "the sliding tyre buzzes on its side");
    Require(haptics.voices.slipHz > 70.0f, "a fast slide buzzes higher");

    // At the limiter with the throttle down the cut chops the engine.
    VehicleAudioHapticsInput limiter = Rolling(20.0f, 6950.0f);
    limiter.rightTrigger = 1.0f;
    haptics = ComputeVehicleAudioHaptics(settings, limiter, slideState, kDt);
    Require(haptics.voices.limiter, "the limiter cuts in");

    // A gear change knocks both hands.
    VehicleAudioHapticsInput shifted = limiter;
    shifted.telemetry.gear = 4;
    haptics = ComputeVehicleAudioHaptics(settings, shifted, slideState, kDt);
    Require(haptics.kicks[0] > 0.0f && haptics.kicks[1] > 0.0f, "an upshift thumps");

    // The clutch is open while the gears change; the drive comes back when it bites.
    shifted.telemetry.clutch = 0.0f;
    haptics = ComputeVehicleAudioHaptics(settings, shifted, slideState, kDt);
    Require(haptics.kicks[0] == 0.0f, "nothing while the clutch is open");
    shifted.telemetry.clutch = 0.4f;
    haptics = ComputeVehicleAudioHaptics(settings, shifted, slideState, kDt);
    Require(haptics.kicks[0] == 0.0f, "nor while it only starts to bite");
    shifted.telemetry.clutch = 0.8f;
    haptics = ComputeVehicleAudioHaptics(settings, shifted, slideState, kDt);
    Require(haptics.kicks[0] > 0.7f && haptics.kicks[1] > 0.7f, "the drive coming back under full throttle shunts");
    shifted.telemetry.clutch = 1.0f;
    haptics = ComputeVehicleAudioHaptics(settings, shifted, slideState, kDt);
    Require(haptics.kicks[0] == 0.0f, "once per change");

    // A quick box: the clutch is shut again by the next frame.
    VehicleAudioHapticsInput quick = shifted;
    quick.rightTrigger = 0.0f;
    quick.telemetry.gear = 5;
    ComputeVehicleAudioHaptics(settings, quick, slideState, kDt);
    haptics = ComputeVehicleAudioHaptics(settings, quick, slideState, kDt);
    Require(std::abs(haptics.kicks[0] - 0.3f) < 1.0e-4f, "off the throttle the drive comes back softly");

    settings.audioHaptics = false;
    haptics = ComputeVehicleAudioHaptics(settings, slide, slideState, kDt);
    Require(haptics.voices == HapticsVoices{} && haptics.kicks[0] == 0.0f, "switched off, the actuators are still");
}

VehicleHapticsInput Driving(int gear, float rpm)
{
    VehicleHapticsInput input;
    input.telemetry.gear = gear;
    input.telemetry.engineRpm = rpm;
    input.telemetry.forwardSpeed = 20.0f;
    input.adaptiveTriggers = true;
    return input;
}

void EngineRumbleFollowsTheRevs()
{
    VehicleHapticsSettings settings;
    settings.rumbleStrength = 1.0f;
    VehicleHapticsState state;
    GamepadFeedback idle;
    GamepadFeedback high;
    for (int frame = 0; frame < 60; ++frame)
    {
        idle = ComputeVehicleFeedback(settings, Driving(3, 1000.0f), state, 1.0f / 60.0f);
    }
    for (int frame = 0; frame < 60; ++frame)
    {
        high = ComputeVehicleFeedback(settings, Driving(3, 6000.0f), state, 1.0f / 60.0f);
    }
    Require(idle.lowFrequencyMotor > 0.0f, "an engine at idle still rumbles a little");
    Require(high.lowFrequencyMotor > idle.lowFrequencyMotor, "higher revs rumble harder");

    VehicleHapticsState offState;
    const GamepadFeedback stopped = ComputeVehicleFeedback(settings, Driving(0, 0.0f), offState, 1.0f / 60.0f);
    Require(stopped.lowFrequencyMotor == 0.0f, "a stopped engine does not rumble");
}

void GearChangesThump()
{
    VehicleHapticsSettings settings;
    settings.rumbleStrength = 1.0f;
    VehicleHapticsState state;
    GamepadFeedback steady;
    for (int frame = 0; frame < 30; ++frame)
    {
        steady = ComputeVehicleFeedback(settings, Driving(2, 4000.0f), state, 1.0f / 60.0f);
    }
    const GamepadFeedback up = ComputeVehicleFeedback(settings, Driving(3, 4000.0f), state, 1.0f / 60.0f);
    Require(up.lowFrequencyMotor > steady.lowFrequencyMotor + 0.2f, "an upshift thumps the big motor");
    Require(up.highFrequencyMotor > 0.0f, "an upshift thumps the small motor too");

    GamepadFeedback later;
    for (int frame = 0; frame < 30; ++frame)
    {
        later = ComputeVehicleFeedback(settings, Driving(3, 4000.0f), state, 1.0f / 60.0f);
    }
    Require(later.highFrequencyMotor == 0.0f, "the thump dies away");

    // Moving off from neutral is not a gear change to feel.
    VehicleHapticsState neutral;
    ComputeVehicleFeedback(settings, Driving(0, 1000.0f), neutral, 1.0f / 60.0f);
    const GamepadFeedback launch = ComputeVehicleFeedback(settings, Driving(1, 1000.0f), neutral, 1.0f / 60.0f);
    Require(launch.highFrequencyMotor == 0.0f, "neutral to first does not thump");
}

void BrakeTriggerShakesWhenAWheelLocks()
{
    const VehicleHapticsSettings settings;
    VehicleHapticsState state;
    VehicleHapticsInput input = Driving(3, 3000.0f);
    input.leftTrigger = 0.9f;
    GamepadFeedback gripping = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    Require(gripping.leftTrigger.mode == TriggerEffect::Mode::Resistance, "the brake pushes back while it grips");

    input.telemetry.lockSlip = 0.4f;
    const GamepadFeedback locking = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    Require(locking.leftTrigger.mode == TriggerEffect::Mode::Vibration, "a locking wheel shakes the brake");
    Require(locking.leftTrigger.frequencyHz > 0, "with a frequency");

    input.telemetry.lockSlip = 0.0f;
    const GamepadFeedback held = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    Require(held.leftTrigger.mode == TriggerEffect::Mode::Vibration, "the shake lasts a moment past the lock");
    GamepadFeedback released;
    for (int frame = 0; frame < 20; ++frame)
    {
        released = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    }
    Require(released.leftTrigger.mode == TriggerEffect::Mode::Resistance, "and then stops");

    input.leftTrigger = 0.0f;
    input.telemetry.lockSlip = 0.5f;
    VehicleHapticsState fresh;
    const GamepadFeedback notBraking = ComputeVehicleFeedback(settings, input, fresh, 1.0f / 60.0f);
    Require(notBraking.leftTrigger.mode == TriggerEffect::Mode::Resistance, "a released brake does not shake");
}

void SwitchesTurnEffectsOff()
{
    VehicleHapticsSettings settings;
    settings.enabled = false;
    VehicleHapticsState state;
    const GamepadFeedback off = ComputeVehicleFeedback(settings, Driving(3, 5000.0f), state, 1.0f / 60.0f);
    Require(off.IsIdle(), "disabled feedback is idle");

    settings.enabled = true;
    settings.triggerStrength = 0.0f;
    VehicleHapticsInput input = Driving(3, 5000.0f);
    input.rightTrigger = 1.0f;
    const GamepadFeedback noTriggers = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    Require(noTriggers.rightTrigger.mode == TriggerEffect::Mode::Off && noTriggers.leftTrigger.mode == TriggerEffect::Mode::Off, "a trigger strength of 0 frees the triggers");

    settings.triggerStrength = 1.0f;
    settings.rumbleStrength = 1.0f;
    input.adaptiveTriggers = false;
    const GamepadFeedback plainPad = ComputeVehicleFeedback(settings, input, state, 1.0f / 60.0f);
    Require(plainPad.rightTrigger.mode == TriggerEffect::Mode::Off && plainPad.lowFrequencyMotor > 0.0f, "a pad without adaptive triggers gets the rumble alone");
}
}

int main()
{
    try
    {
        EncodesTheEffectsState();
        IdleFreesEverything();
        AudioHapticsLeaveTheRumbleEmulationOff();
        AudioHapticsFollowTheCar();
        EngineRumbleFollowsTheRevs();
        GearChangesThump();
        BrakeTriggerShakesWhenAWheelLocks();
        SwitchesTurnEffectsOff();
    }
    catch (const std::exception& error)
    {
        std::cerr << "gamepad feedback test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "gamepad feedback tests passed\n";
    return 0;
}
