#include "gamepad_backend.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace me
{

namespace
{
constexpr size_t kMaxGamepads = 4;
// The dead zones XInput recommends, as fractions of full travel: the sticks rest off centre and the
// triggers rest slightly pressed on some pads. Every pad gets them, so an Xbox pad reads as it did
// through XInput.
constexpr float kLeftStickDeadZone = 7849.0f / 32767.0f;
constexpr float kRightStickDeadZone = 8689.0f / 32767.0f;
constexpr float kTriggerThreshold = 30.0f / 255.0f;
// The most often feedback goes to a pad, and how long before an unchanged one is sent again.
constexpr std::chrono::milliseconds kMinSendInterval{8};
constexpr std::chrono::milliseconds kRefreshInterval{500};
// A plain rumble stops by itself after this long, so it is renewed rather than left running.
constexpr Uint32 kRumbleDurationMs = 600;

using Clock = std::chrono::steady_clock;

struct SdlGamepadSlot
{
    bool assigned = false;
    SDL_JoystickID joystickId = 0;
    SDL_Gamepad* handle = nullptr;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    uint32_t packetNumber = 0;
    std::array<bool, PolledGamepadState::kButtonCount> buttonDown{};
    std::array<float, PolledGamepadState::kAxisCount> axisValues{};

    // The last feedback sent and when, so an unchanged one is not sent again.
    bool feedbackSent = false;
    GamepadFeedback lastFeedback;
    Clock::time_point lastSendTime{};
    bool reportedSendFailure = false;
    bool reportedFirstSend = false;
};

std::array<SdlGamepadSlot, kMaxGamepads>& GetSdlGamepadSlots()
{
    static std::array<SdlGamepadSlot, kMaxGamepads> slots{};
    return slots;
}

bool IsDualSense(SDL_GamepadType type)
{
    return type == SDL_GAMEPAD_TYPE_PS5;
}

bool SendToGamepad(SdlGamepadSlot& slot, const GamepadFeedback& feedback)
{
    if (IsDualSense(slot.type))
    {
        const std::array<uint8_t, kDualSenseEffectsSize> effects = EncodeDualSenseEffects(feedback);
        return SDL_SendGamepadEffect(slot.handle, effects.data(), static_cast<int>(effects.size()));
    }

    // Any other pad: the two motors, if it has them. A trigger effect has nowhere to go.
    const auto motor = [](float value)
    { return static_cast<Uint16>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f)); };
    return SDL_RumbleGamepad(slot.handle, motor(feedback.lowFrequencyMotor), motor(feedback.highFrequencyMotor), kRumbleDurationMs);
}

void ResetSdlGamepadSlot(SdlGamepadSlot& slot)
{
    if (slot.handle != nullptr)
    {
        if (slot.feedbackSent && !slot.lastFeedback.IsIdle())
        {
            // Closing the pad does not stop what it was told to do.
            SendToGamepad(slot, GamepadFeedback{});
        }
        SDL_CloseGamepad(slot.handle);
    }

    slot = SdlGamepadSlot{};
}

float ApplyAxialDeadZone(float normalized, float deadZone)
{
    const float magnitude = std::abs(normalized);
    if (magnitude <= deadZone)
    {
        return 0.0f;
    }

    return std::clamp(std::copysign((magnitude - deadZone) / (1.0f - deadZone), normalized), -1.0f, 1.0f);
}

float NormalizeSdlGamepadAxis(SDL_GamepadAxis axis, Sint16 value)
{
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
    {
        const float pressed = std::clamp(static_cast<float>(std::max<Sint16>(value, 0)) / 32767.0f, 0.0f, 1.0f);
        return pressed <= kTriggerThreshold ? 0.0f : (pressed - kTriggerThreshold) / (1.0f - kTriggerThreshold);
    }

    const float maxMagnitude = value < 0 ? 32768.0f : 32767.0f;
    float normalized = maxMagnitude > 0.0f ? static_cast<float>(value) / maxMagnitude : 0.0f;

    // SDL already reports a stick pushed up as negative, the sign the engine reads (what XInput gave once flipped).
    const bool left = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
    normalized = ApplyAxialDeadZone(normalized, left ? kLeftStickDeadZone : kRightStickDeadZone);

    return std::clamp(normalized, -1.0f, 1.0f);
}
}

std::array<PolledGamepadState, 4> PollPlatformGamepads()
{
    std::array<PolledGamepadState, 4> gamepads{};

    std::array<SdlGamepadSlot, kMaxGamepads>& slots = GetSdlGamepadSlots();
    std::array<bool, kMaxGamepads> slotSeen{};

    int connectedGamepadCount = 0;
    SDL_JoystickID* connectedGamepads = SDL_GetGamepads(&connectedGamepadCount);

    for (int connectedIndex = 0; connectedIndex < connectedGamepadCount; ++connectedIndex)
    {
        const SDL_JoystickID joystickId = connectedGamepads[connectedIndex];

        size_t slotIndex = slots.size();
        for (size_t existingSlotIndex = 0; existingSlotIndex < slots.size(); ++existingSlotIndex)
        {
            if (slots[existingSlotIndex].assigned && slots[existingSlotIndex].joystickId == joystickId)
            {
                slotIndex = existingSlotIndex;
                break;
            }
        }

        if (slotIndex == slots.size())
        {
            for (size_t freeSlotIndex = 0; freeSlotIndex < slots.size(); ++freeSlotIndex)
            {
                if (!slots[freeSlotIndex].assigned)
                {
                    slots[freeSlotIndex].assigned = true;
                    slots[freeSlotIndex].joystickId = joystickId;
                    slots[freeSlotIndex].handle = SDL_OpenGamepad(joystickId);
                    slotIndex = freeSlotIndex;
                    break;
                }
            }
        }

        if (slotIndex == slots.size())
        {
            continue;
        }

        SdlGamepadSlot& slot = slots[slotIndex];
        if (slot.handle == nullptr)
        {
            slot.handle = SDL_OpenGamepad(joystickId);
        }

        if (slot.handle == nullptr)
        {
            ResetSdlGamepadSlot(slot);
            continue;
        }

        if (slot.type == SDL_GAMEPAD_TYPE_UNKNOWN)
        {
            slot.type = SDL_GetGamepadType(slot.handle);
            LOG_INFO("Gamepad {} connected: {} (type {})", slotIndex, SDL_GetGamepadName(slot.handle), static_cast<int>(slot.type));
        }

        slotSeen[slotIndex] = true;

        PolledGamepadState& gamepad = gamepads[slotIndex];
        gamepad.connected = true;
        gamepad.type = slot.type;

        for (size_t buttonIndex = 0; buttonIndex < gamepad.buttonDown.size(); ++buttonIndex)
        {
            gamepad.buttonDown[buttonIndex] =
                SDL_GetGamepadButton(slot.handle, static_cast<SDL_GamepadButton>(buttonIndex)) != 0;
        }

        for (size_t axisIndex = 0; axisIndex < gamepad.axisValues.size(); ++axisIndex)
        {
            const SDL_GamepadAxis axis = static_cast<SDL_GamepadAxis>(axisIndex);
            gamepad.axisValues[axisIndex] = NormalizeSdlGamepadAxis(axis, SDL_GetGamepadAxis(slot.handle, axis));
        }

        if (gamepad.buttonDown != slot.buttonDown || gamepad.axisValues != slot.axisValues)
        {
            ++slot.packetNumber;
            slot.buttonDown = gamepad.buttonDown;
            slot.axisValues = gamepad.axisValues;
        }

        gamepad.packetNumber = slot.packetNumber;
    }

    if (connectedGamepads != nullptr)
    {
        SDL_free(connectedGamepads);
    }

    for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
    {
        if (!slotSeen[slotIndex])
        {
            ResetSdlGamepadSlot(slots[slotIndex]);
        }
    }

    return gamepads;
}

void SendPlatformGamepadFeedback(size_t playerIndex, const GamepadFeedback& feedback)
{
    std::array<SdlGamepadSlot, kMaxGamepads>& slots = GetSdlGamepadSlots();
    if (playerIndex >= slots.size() || slots[playerIndex].handle == nullptr)
    {
        return;
    }

    SdlGamepadSlot& slot = slots[playerIndex];
    const Clock::time_point now = Clock::now();
    const bool changed = !slot.feedbackSent || slot.lastFeedback != feedback;
    if (!changed && feedback.IsIdle())
    {
        return;
    }
    if (slot.feedbackSent)
    {
        const Clock::duration sinceSend = now - slot.lastSendTime;
        // Going quiet is never held back; anything else waits out the minimum interval.
        const bool goingIdle = changed && feedback.IsIdle();
        if (changed ? (!goingIdle && sinceSend < kMinSendInterval) : sinceSend < kRefreshInterval)
        {
            return;
        }
    }
    else if (feedback.IsIdle())
    {
        // Nothing was ever asked of this pad: leave it alone.
        slot.feedbackSent = true;
        slot.lastFeedback = feedback;
        slot.lastSendTime = now;
        return;
    }

    if (!slot.reportedFirstSend && !feedback.IsIdle())
    {
        slot.reportedFirstSend = true;
        LOG_INFO("Gamepad {} feedback started ({}): motors {:.2f}/{:.2f}, triggers L{} R{}", playerIndex, IsDualSense(slot.type) ? "DualSense effects" : "rumble only",
                 feedback.lowFrequencyMotor, feedback.highFrequencyMotor, static_cast<int>(feedback.leftTrigger.mode), static_cast<int>(feedback.rightTrigger.mode));
    }

    if (SendToGamepad(slot, feedback))
    {
        slot.reportedSendFailure = false;
    }
    else if (!slot.reportedSendFailure)
    {
        slot.reportedSendFailure = true;
        LOG_WARN("Gamepad {} feedback was refused: {}", playerIndex, SDL_GetError());
    }

    slot.feedbackSent = true;
    slot.lastFeedback = feedback;
    slot.lastSendTime = now;
}

void ReleasePlatformGamepads()
{
    for (SdlGamepadSlot& slot : GetSdlGamepadSlots())
    {
        ResetSdlGamepadSlot(slot);
    }
}
}
