#include "gamepad_backend.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>
#endif

namespace
{
#ifdef _WIN32
float NormalizeSignedThumbAxis(short value, short deadzone)
{
    const float maxMagnitude = value < 0 ? 32768.0f : 32767.0f;
    const float normalized = static_cast<float>(value) / maxMagnitude;
    const float normalizedDeadzone = static_cast<float>(deadzone) / 32767.0f;

    if (std::abs(normalized) <= normalizedDeadzone)
    {
        return 0.0f;
    }

    const float direction = normalized < 0.0f ? -1.0f : 1.0f;
    const float magnitude = (std::abs(normalized) - normalizedDeadzone) / (1.0f - normalizedDeadzone);
    return std::clamp(magnitude * direction, -1.0f, 1.0f);
}

float NormalizeTriggerAxis(unsigned char value)
{
    if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
    {
        return 0.0f;
    }

    return std::clamp(
        static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
            static_cast<float>(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD),
        0.0f,
        1.0f
    );
}

void SetButtonState(
    PolledGamepadState& gamepad,
    SDL_GamepadButton button,
    WORD buttons,
    WORD mask
)
{
    const size_t buttonIndex = static_cast<size_t>(button);
    if (buttonIndex >= gamepad.buttonDown.size())
    {
        return;
    }

    gamepad.buttonDown[buttonIndex] = (buttons & mask) != 0;
}
#endif
}

std::array<PolledGamepadState, 4> PollPlatformGamepads()
{
    std::array<PolledGamepadState, 4> gamepads{};

#if defined(_WIN32)
    for (uint32_t playerIndex = 0; playerIndex < gamepads.size(); ++playerIndex)
    {
        XINPUT_STATE state{};
        const DWORD result = XInputGetState(playerIndex, &state);
        if (result != ERROR_SUCCESS)
        {
            continue;
        }

        PolledGamepadState& gamepad = gamepads[playerIndex];
        gamepad.connected = true;
        gamepad.packetNumber = state.dwPacketNumber;

        const XINPUT_GAMEPAD& xinputGamepad = state.Gamepad;
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_SOUTH, xinputGamepad.wButtons, XINPUT_GAMEPAD_A);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_EAST, xinputGamepad.wButtons, XINPUT_GAMEPAD_B);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_WEST, xinputGamepad.wButtons, XINPUT_GAMEPAD_X);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_NORTH, xinputGamepad.wButtons, XINPUT_GAMEPAD_Y);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_BACK, xinputGamepad.wButtons, XINPUT_GAMEPAD_BACK);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_START, xinputGamepad.wButtons, XINPUT_GAMEPAD_START);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK, xinputGamepad.wButtons, XINPUT_GAMEPAD_LEFT_THUMB);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK, xinputGamepad.wButtons, XINPUT_GAMEPAD_RIGHT_THUMB);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, xinputGamepad.wButtons, XINPUT_GAMEPAD_LEFT_SHOULDER);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, xinputGamepad.wButtons, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP, xinputGamepad.wButtons, XINPUT_GAMEPAD_DPAD_UP);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, xinputGamepad.wButtons, XINPUT_GAMEPAD_DPAD_DOWN);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, xinputGamepad.wButtons, XINPUT_GAMEPAD_DPAD_LEFT);
        SetButtonState(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, xinputGamepad.wButtons, XINPUT_GAMEPAD_DPAD_RIGHT);

        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_LEFTX)] =
            NormalizeSignedThumbAxis(xinputGamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_LEFTY)] =
            -NormalizeSignedThumbAxis(xinputGamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_RIGHTX)] =
            NormalizeSignedThumbAxis(xinputGamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_RIGHTY)] =
            -NormalizeSignedThumbAxis(xinputGamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)] =
            NormalizeTriggerAxis(xinputGamepad.bLeftTrigger);
        gamepad.axisValues[static_cast<size_t>(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)] =
            NormalizeTriggerAxis(xinputGamepad.bRightTrigger);
    }
#endif

    return gamepads;
}
