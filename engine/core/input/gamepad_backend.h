#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>

struct PolledGamepadState
{
    static constexpr size_t kButtonCount = static_cast<size_t>(SDL_GAMEPAD_BUTTON_COUNT);
    static constexpr size_t kAxisCount = static_cast<size_t>(SDL_GAMEPAD_AXIS_COUNT);

    bool connected = false;
    uint32_t packetNumber = 0;
    std::array<bool, kButtonCount> buttonDown{};
    std::array<float, kAxisCount> axisValues{};
};

std::array<PolledGamepadState, 4> PollPlatformGamepads();
