#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

struct KeyCode
{
    constexpr KeyCode() = default;
    constexpr explicit KeyCode(uint16_t rawValue)
        : value(rawValue)
    {
    }
    constexpr explicit KeyCode(SDL_Scancode scancode)
        : value(static_cast<uint16_t>(scancode))
    {
    }

    uint16_t value = static_cast<uint16_t>(SDL_SCANCODE_UNKNOWN);
};

namespace KeyCodes
{
inline constexpr KeyCode Unknown{ SDL_SCANCODE_UNKNOWN };
inline constexpr KeyCode W{ SDL_SCANCODE_W };
inline constexpr KeyCode A{ SDL_SCANCODE_A };
inline constexpr KeyCode S{ SDL_SCANCODE_S };
inline constexpr KeyCode D{ SDL_SCANCODE_D };
inline constexpr KeyCode Q{ SDL_SCANCODE_Q };
inline constexpr KeyCode E{ SDL_SCANCODE_E };
inline constexpr KeyCode Space{ SDL_SCANCODE_SPACE };
inline constexpr KeyCode LeftShift{ SDL_SCANCODE_LSHIFT };
inline constexpr KeyCode RightShift{ SDL_SCANCODE_RSHIFT };
inline constexpr KeyCode LeftControl{ SDL_SCANCODE_LCTRL };
inline constexpr KeyCode RightControl{ SDL_SCANCODE_RCTRL };
}

enum class GamepadButton : int8_t
{
    Invalid = SDL_GAMEPAD_BUTTON_INVALID,
    South = SDL_GAMEPAD_BUTTON_SOUTH,
    East = SDL_GAMEPAD_BUTTON_EAST,
    West = SDL_GAMEPAD_BUTTON_WEST,
    North = SDL_GAMEPAD_BUTTON_NORTH,
    Back = SDL_GAMEPAD_BUTTON_BACK,
    Guide = SDL_GAMEPAD_BUTTON_GUIDE,
    Start = SDL_GAMEPAD_BUTTON_START,
    LeftStick = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    RightStick = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    LeftShoulder = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    RightShoulder = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    DpadUp = SDL_GAMEPAD_BUTTON_DPAD_UP,
    DpadDown = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    DpadLeft = SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    DpadRight = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    Misc1 = SDL_GAMEPAD_BUTTON_MISC1,
    RightPaddle1 = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    LeftPaddle1 = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    RightPaddle2 = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    LeftPaddle2 = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    Touchpad = SDL_GAMEPAD_BUTTON_TOUCHPAD,
    Misc2 = SDL_GAMEPAD_BUTTON_MISC2,
    Misc3 = SDL_GAMEPAD_BUTTON_MISC3,
    Misc4 = SDL_GAMEPAD_BUTTON_MISC4,
    Misc5 = SDL_GAMEPAD_BUTTON_MISC5,
    Misc6 = SDL_GAMEPAD_BUTTON_MISC6,
};

enum class GamepadAxis : int8_t
{
    Invalid = SDL_GAMEPAD_AXIS_INVALID,
    LeftX = SDL_GAMEPAD_AXIS_LEFTX,
    LeftY = SDL_GAMEPAD_AXIS_LEFTY,
    RightX = SDL_GAMEPAD_AXIS_RIGHTX,
    RightY = SDL_GAMEPAD_AXIS_RIGHTY,
    LeftTrigger = SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    RightTrigger = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

class InputState
{
public:
    static constexpr size_t kKeyboardKeyCount = static_cast<size_t>(SDL_SCANCODE_COUNT);
    static constexpr size_t kGamepadButtonCount = static_cast<size_t>(SDL_GAMEPAD_BUTTON_COUNT);
    static constexpr size_t kGamepadAxisCount = static_cast<size_t>(SDL_GAMEPAD_AXIS_COUNT);
    static constexpr uint32_t kMaxGamepads = 4;

    void HandleEvent(const SDL_Event& event);
    void Update();
    void EndFrame();
    void SetViewportInteractionRegion(const SDL_FRect& rect, bool enabled);

    bool IsKeyDown(KeyCode key) const;
    bool WasKeyPressed(KeyCode key) const;
    bool WasKeyReleased(KeyCode key) const;
    bool WasKeyRepeated(KeyCode key) const;

    bool IsKeyDown(SDL_Scancode scancode) const;
    bool WasKeyPressed(SDL_Scancode scancode) const;
    bool WasKeyReleased(SDL_Scancode scancode) const;
    bool WasKeyRepeated(SDL_Scancode scancode) const;

    bool IsGamepadConnected(uint32_t playerIndex = 0) const;
    bool WasGamepadConnected(uint32_t playerIndex = 0) const;
    bool WasGamepadDisconnected(uint32_t playerIndex = 0) const;
    bool IsGamepadButtonDown(GamepadButton button, uint32_t playerIndex = 0) const;
    bool WasGamepadButtonPressed(GamepadButton button, uint32_t playerIndex = 0) const;
    bool WasGamepadButtonReleased(GamepadButton button, uint32_t playerIndex = 0) const;
    float GetGamepadAxis(GamepadAxis axis, uint32_t playerIndex = 0) const;
    float GetGamepadAxisDelta(GamepadAxis axis, uint32_t playerIndex = 0) const;
    bool WasGamepadAxisChanged(GamepadAxis axis, uint32_t playerIndex = 0) const;
    int GetFirstConnectedGamepadIndex() const;

    bool IsMouseLookActive() const;
    bool IsMousePanActive() const;
    bool WantsRelativeMouseMode() const;
    float GetMouseDeltaX() const;
    float GetMouseDeltaY() const;
    bool ShouldRestoreMouseLookAnchor() const;
    void ConsumeMouseLookAnchor(int& x, int& y);

    static KeyCode FromScancode(SDL_Scancode scancode);
    static std::string GetKeyName(KeyCode key);
    static std::string GetGamepadButtonName(GamepadButton button);
    static std::string GetGamepadAxisName(GamepadAxis axis);

private:
    struct GamepadState
    {
        bool connected = false;
        bool connectedThisFrame = false;
        bool disconnectedThisFrame = false;
        uint32_t packetNumber = 0;
        std::array<bool, kGamepadButtonCount> buttonDown{};
        std::array<bool, kGamepadButtonCount> buttonPressed{};
        std::array<bool, kGamepadButtonCount> buttonReleased{};
        std::array<float, kGamepadAxisCount> axisValues{};
        std::array<float, kGamepadAxisCount> axisDeltas{};
        std::array<bool, kGamepadAxisCount> axisChanged{};
    };

    static size_t ToKeyIndex(KeyCode key);
    static size_t ToGamepadButtonIndex(GamepadButton button);
    static size_t ToGamepadAxisIndex(GamepadAxis axis);

    bool IsValidKeyCode(KeyCode key) const;
    bool IsValidGamepadPlayerIndex(uint32_t playerIndex) const;
    bool IsValidGamepadButton(GamepadButton button) const;
    bool IsValidGamepadAxis(GamepadAxis axis) const;
    bool IsViewportInteractionPoint(float x, float y) const;
    void PollGamepads();
    void EnsureTimestampBaseInitialized();
    std::string FormatEventTimestamp(Uint64 timestampNs) const;
    void LogKeyboardEvent(Uint64 timestampNs, KeyCode key, const char* action) const;
    void LogGamepadConnectionEvent(Uint64 timestampNs, uint32_t playerIndex, const char* action) const;
    void LogGamepadButtonEvent(Uint64 timestampNs, uint32_t playerIndex, GamepadButton button, const char* action) const;
    void LogGamepadAxisEvent(Uint64 timestampNs, uint32_t playerIndex, GamepadAxis axis, float value) const;

    std::array<bool, kKeyboardKeyCount> m_keyDown{};
    std::array<bool, kKeyboardKeyCount> m_keyPressed{};
    std::array<bool, kKeyboardKeyCount> m_keyReleased{};
    std::array<bool, kKeyboardKeyCount> m_keyRepeated{};
    std::array<GamepadState, kMaxGamepads> m_gamepads{};
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
    bool m_mouseLookActive = false;
    bool m_mousePanActive = false;
    bool m_viewportInteractionEnabled = false;
    bool m_hasMouseLookAnchor = false;
    bool m_shouldRestoreMouseLookAnchor = false;
    int m_mouseLookAnchorX = 0;
    int m_mouseLookAnchorY = 0;
    SDL_FRect m_viewportInteractionRect{ 0.0f, 0.0f, 0.0f, 0.0f };
    bool m_hasTimestampBase = false;
    std::chrono::system_clock::time_point m_wallClockAtSdlTickZero{};
};
