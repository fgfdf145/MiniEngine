#include "gamepad_feedback.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// The DualSense's effects state, byte by byte (the layout SDL's own driver uses).
constexpr size_t kEnableBits1 = 0;
constexpr size_t kRumbleRight = 2;
constexpr size_t kRumbleLeft = 3;
constexpr size_t kRightTriggerEffect = 10;
constexpr size_t kLeftTriggerEffect = 21;

constexpr uint8_t kEnableCompatibleRumble = 0x01;
constexpr uint8_t kDisableAudioHaptics = 0x02;
constexpr uint8_t kEnableRightTrigger = 0x04;
constexpr uint8_t kEnableLeftTrigger = 0x08;

// The trigger effect modes: the first byte of a trigger's 11.
constexpr uint8_t kModeOff = 0x00;
constexpr uint8_t kModeMultiPositionFeedback = 0x21;
constexpr uint8_t kModeMultiPositionVibration = 0x26;

uint8_t ToByte(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
}

bool GamepadFeedback::IsIdle() const
{
    return !audioHaptics && lowFrequencyMotor <= 0.0f && highFrequencyMotor <= 0.0f && leftTrigger.mode == TriggerEffect::Mode::Off &&
           rightTrigger.mode == TriggerEffect::Mode::Off;
}

std::array<uint8_t, 11> EncodeDualSenseTriggerEffect(const TriggerEffect& effect)
{
    std::array<uint8_t, 11> bytes{};
    if (effect.mode == TriggerEffect::Mode::Off)
    {
        bytes[0] = kModeOff;
        return bytes;
    }

    // Bit n of the mask turns zone n on; its strength less one takes three bits of the 32-bit word.
    uint16_t zoneMask = 0;
    uint32_t strengths = 0;
    for (size_t zone = 0; zone < kTriggerZoneCount; ++zone)
    {
        const uint8_t strength = std::min(effect.zoneStrength[zone], kTriggerMaxStrength);
        if (strength == 0)
        {
            continue;
        }
        zoneMask = static_cast<uint16_t>(zoneMask | (1u << zone));
        strengths |= static_cast<uint32_t>(strength - 1) << (3 * zone);
    }
    if (zoneMask == 0)
    {
        bytes[0] = kModeOff;
        return bytes;
    }

    const bool vibration = effect.mode == TriggerEffect::Mode::Vibration;
    bytes[0] = vibration ? kModeMultiPositionVibration : kModeMultiPositionFeedback;
    bytes[1] = static_cast<uint8_t>(zoneMask & 0xFF);
    bytes[2] = static_cast<uint8_t>(zoneMask >> 8);
    for (size_t index = 0; index < 4; ++index)
    {
        bytes[3 + index] = static_cast<uint8_t>((strengths >> (8 * index)) & 0xFF);
    }
    if (vibration)
    {
        bytes[9] = effect.frequencyHz;
    }
    return bytes;
}

std::array<uint8_t, kDualSenseEffectsSize> EncodeDualSenseEffects(const GamepadFeedback& feedback)
{
    std::array<uint8_t, kDualSenseEffectsSize> state{};
    state[kEnableBits1] = kEnableRightTrigger | kEnableLeftTrigger;
    if (!feedback.audioHaptics)
    {
        // Leaving these two bits off is what gives the actuators back to the audio.
        state[kEnableBits1] |= kEnableCompatibleRumble | kDisableAudioHaptics;
        // The left motor is the big, low frequency one.
        state[kRumbleLeft] = ToByte(feedback.lowFrequencyMotor);
        state[kRumbleRight] = ToByte(feedback.highFrequencyMotor);
    }

    const std::array<uint8_t, 11> right = EncodeDualSenseTriggerEffect(feedback.rightTrigger);
    const std::array<uint8_t, 11> left = EncodeDualSenseTriggerEffect(feedback.leftTrigger);
    std::copy(right.begin(), right.end(), state.begin() + kRightTriggerEffect);
    std::copy(left.begin(), left.end(), state.begin() + kLeftTriggerEffect);
    return state;
}
}
