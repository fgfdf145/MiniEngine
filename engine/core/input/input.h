#pragma once

#include <engine/core/input/gamepad_feedback.h>

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace me
{

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
inline constexpr KeyCode W{SDL_SCANCODE_W};
inline constexpr KeyCode A{SDL_SCANCODE_A};
inline constexpr KeyCode S{SDL_SCANCODE_S};
inline constexpr KeyCode D{SDL_SCANCODE_D};
inline constexpr KeyCode LeftAlt{SDL_SCANCODE_LALT};
inline constexpr KeyCode RightAlt{SDL_SCANCODE_RALT};
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
    // The viewport's picture on the window and whether it takes the mouse at all. hovered: whether the
    // picture is what the UI has under the mouse (no window over it there); presses and the wheel over
    // a window that covers the picture are that window's. A look or pan already going on keeps going.
    void SetViewportInteractionRegion(const SDL_FRect& rect, bool enabled, bool hovered = true);

    bool IsKeyDown(KeyCode key) const;

    float GetGamepadAxis(GamepadAxis axis, uint32_t playerIndex = 0) const;
    bool IsGamepadButtonDown(GamepadButton button, uint32_t playerIndex = 0) const;
    int GetFirstConnectedGamepadIndex() const;
    // What kind of pad is in this slot (SDL_GAMEPAD_TYPE_PS5 for a DualSense); unknown when none.
    SDL_GamepadType GetGamepadType(uint32_t playerIndex = 0) const;
    // Makes the pad do `feedback` (rumble, and a DualSense's adaptive triggers) until told otherwise.
    // It is sent when it changes, so calling this every frame is fine.
    void SetGamepadFeedback(uint32_t playerIndex, const GamepadFeedback& feedback);
    // Stops every pad's rumble and frees its triggers.
    void ClearGamepadFeedback();

    bool IsMouseLookActive() const;
    bool IsMousePanActive() const;
    bool WantsRelativeMouseMode() const;
    float GetMouseDeltaX() const;
    float GetMouseDeltaY() const;
    float GetMouseWheelDelta() const;
    bool ShouldRestoreMouseLookAnchor() const;
    void ConsumeMouseLookAnchor(int& x, int& y);
    // True once after a right click in the viewport that was a click, not a mouse look: the
    // button came up quickly without the mouse moving.
    bool ConsumeViewportRightClick();

    static KeyCode FromScancode(SDL_Scancode scancode);
    static std::string GetKeyName(KeyCode key);
    static std::string GetGamepadButtonName(GamepadButton button);
    static std::string GetGamepadAxisName(GamepadAxis axis);

  private:
    struct GamepadState
    {
        bool connected = false;
        SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
        uint32_t packetNumber = 0;
        std::array<bool, kGamepadButtonCount> buttonDown{};
        std::array<float, kGamepadAxisCount> axisValues{};
    };

    static size_t ToKeyIndex(KeyCode key);
    static size_t ToGamepadAxisIndex(GamepadAxis axis);

    bool IsValidKeyCode(KeyCode key) const;
    bool IsValidGamepadPlayerIndex(uint32_t playerIndex) const;
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
    std::array<GamepadState, kMaxGamepads> m_gamepads{};
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
    float m_mouseWheelDelta = 0.0f;
    bool m_mouseLookActive = false;
    bool m_mousePanActive = false;
    bool m_viewportInteractionEnabled = false;
    bool m_viewportHovered = true;
    bool m_hasMouseLookAnchor = false;
    bool m_shouldRestoreMouseLookAnchor = false;
    int m_mouseLookAnchorX = 0;
    int m_mouseLookAnchorY = 0;
    float m_mouseLookTravel = 0.0f;
    Uint64 m_mouseLookPressTimestampNs = 0;
    bool m_viewportRightClicked = false;
    SDL_FRect m_viewportInteractionRect{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_hasTimestampBase = false;
    std::chrono::system_clock::time_point m_wallClockAtSdlTickZero{};
};
}
