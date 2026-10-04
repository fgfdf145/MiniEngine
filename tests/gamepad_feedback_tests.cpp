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
    const VehicleHapticsSettings settings;
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
    const VehicleHapticsSettings settings;
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
