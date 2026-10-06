#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace me
{

// How many zones a DualSense trigger's travel is split into for its effects.
inline constexpr size_t kTriggerZoneCount = 10;
inline constexpr uint8_t kTriggerMaxStrength = 8;

// What one adaptive trigger does. Resistance pushes back against the finger in each active zone;
// Vibration shakes it there at frequencyHz. A zone's strength is 0 (the effect is off there) or 1 to
// kTriggerMaxStrength.
struct TriggerEffect
{
    enum class Mode : uint8_t
    {
        Off,
        Resistance,
        Vibration,
    };

    Mode mode = Mode::Off;
    std::array<uint8_t, kTriggerZoneCount> zoneStrength{};
    uint8_t frequencyHz = 0;

    bool operator==(const TriggerEffect&) const = default;
};

// What a gamepad should be doing to the player's hands: the two rumble motors (0 to 1; the low
// frequency one is the big motor on the left of the grip) and the adaptive triggers, which only a
// DualSense has. Devices without a feature ignore it.
struct GamepadFeedback
{
    float lowFrequencyMotor = 0.0f;
    float highFrequencyMotor = 0.0f;
    TriggerEffect leftTrigger;
    TriggerEffect rightTrigger;
    // A DualSense whose actuators are played as sound (GamepadHaptics): its rumble emulation stays off,
    // which hands the actuators to the audio, and the motors above are not used.
    bool audioHaptics = false;

    // Nothing to feel: motors off, both triggers free, and no audio haptics (which have to be asked for).
    bool IsIdle() const;
    bool operator==(const GamepadFeedback&) const = default;
};

// A DualSense's output report body (its effects state, which starts at the report's enable bits)
// is this long. SDL_SendGamepadEffect takes exactly this, and adds the report id and, over Bluetooth,
// the checksum.
inline constexpr size_t kDualSenseEffectsSize = 47;

// The effects state that makes the controller do `feedback`. It always claims both triggers, so an idle
// feedback frees them. Rumble goes through the controller's rumble emulation, which turns the audio
// haptics off; with feedback.audioHaptics the emulation is left off, and the actuators follow the audio.
std::array<uint8_t, kDualSenseEffectsSize> EncodeDualSenseEffects(const GamepadFeedback& feedback);

// One trigger's 11 bytes of the effects state.
std::array<uint8_t, 11> EncodeDualSenseTriggerEffect(const TriggerEffect& effect);
}
