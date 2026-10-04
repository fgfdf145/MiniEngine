#pragma once

#include <engine/core/input/gamepad_feedback.h>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace me
{

struct PolledGamepadState
{
    static constexpr size_t kButtonCount = static_cast<size_t>(SDL_GAMEPAD_BUTTON_COUNT);
    static constexpr size_t kAxisCount = static_cast<size_t>(SDL_GAMEPAD_AXIS_COUNT);

    bool connected = false;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    uint32_t packetNumber = 0;
    std::array<bool, kButtonCount> buttonDown{};
    std::array<float, kAxisCount> axisValues{};
};

std::array<PolledGamepadState, 4> PollPlatformGamepads();

// Makes the gamepad in slot `playerIndex` (as PollPlatformGamepads numbers them) do `feedback`: the
// DualSense's motors and adaptive triggers, or plain rumble on any other pad. Sends only when it
// changed (and now and then regardless, in case the pad dropped it) and at a bounded rate.
void SendPlatformGamepadFeedback(size_t playerIndex, const GamepadFeedback& feedback);

// Frees every gamepad's motors and triggers and closes them. For shutdown.
void ReleasePlatformGamepads();
}
