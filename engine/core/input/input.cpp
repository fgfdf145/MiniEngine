#include "input.h"
#include "gamepad_backend.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace me
{

namespace
{
constexpr float kGamepadAxisChangeEpsilon = 0.001f;
constexpr float kGamepadAxisLogThreshold = 0.10f;
// A right press that moves less than this and is released sooner is a click, not a mouse look.
constexpr float kRightClickMaxTravelPixels = 4.0f;
constexpr Uint64 kRightClickMaxDurationNs = 300'000'000;

bool ShouldLogAxisChange(float previousValue, float currentValue)
{
    if (std::abs(currentValue - previousValue) >= kGamepadAxisLogThreshold)
    {
        return true;
    }

    const bool wasActive = std::abs(previousValue) > kGamepadAxisChangeEpsilon;
    const bool isActive = std::abs(currentValue) > kGamepadAxisChangeEpsilon;
    return wasActive != isActive;
}
}

void InputState::HandleEvent(const SDL_Event& event)
{
    EnsureTimestampBaseInitialized();

    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    {
        const KeyCode key = FromScancode(event.key.scancode);
        if (!IsValidKeyCode(key))
        {
            break;
        }

        const size_t keyIndex = ToKeyIndex(key);
        if (event.key.repeat)
        {
            LogKeyboardEvent(event.key.timestamp, key, "repeated");
            break;
        }

        const bool wasDown = m_keyDown[keyIndex];
        m_keyDown[keyIndex] = true;
        if (!wasDown)
        {
            LogKeyboardEvent(event.key.timestamp, key, "pressed");
        }
        break;
    }
    case SDL_EVENT_KEY_UP:
    {
        const KeyCode key = FromScancode(event.key.scancode);
        if (!IsValidKeyCode(key))
        {
            break;
        }

        m_keyDown[ToKeyIndex(key)] = false;
        LogKeyboardEvent(event.key.timestamp, key, "released");
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        m_mouseDeltaX += event.motion.xrel;
        m_mouseDeltaY += event.motion.yrel;
        if (m_mouseLookActive)
        {
            m_mouseLookTravel += std::abs(event.motion.xrel) + std::abs(event.motion.yrel);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_RIGHT &&
            IsViewportInteractionPoint(event.button.x, event.button.y))
        {
            m_mouseLookActive = true;
            m_hasMouseLookAnchor = true;
            m_mouseLookAnchorX = static_cast<int>(event.button.x);
            m_mouseLookAnchorY = static_cast<int>(event.button.y);
            m_shouldRestoreMouseLookAnchor = false;
            m_mouseLookTravel = 0.0f;
            m_mouseLookPressTimestampNs = event.button.timestamp;
        }
        if (event.button.button == SDL_BUTTON_MIDDLE &&
            IsViewportInteractionPoint(event.button.x, event.button.y))
        {
            m_mousePanActive = true;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (m_mouseLookActive || m_mousePanActive ||
            IsViewportInteractionPoint(event.wheel.mouse_x, event.wheel.mouse_y))
        {
            const float direction =
                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            m_mouseWheelDelta += event.wheel.y * direction;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT)
        {
            m_viewportRightClicked = m_mouseLookActive &&
                                     m_mouseLookTravel < kRightClickMaxTravelPixels &&
                                     event.button.timestamp - m_mouseLookPressTimestampNs < kRightClickMaxDurationNs;
            m_mouseLookActive = false;
            m_shouldRestoreMouseLookAnchor = m_hasMouseLookAnchor;
        }
        if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            m_mousePanActive = false;
        }
        break;
    default:
        break;
    }
}

void InputState::Update()
{
    EnsureTimestampBaseInitialized();
    PollGamepads();
}

void InputState::EndFrame()
{
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
    m_mouseWheelDelta = 0.0f;
}

void InputState::SetViewportInteractionRegion(const SDL_FRect& rect, bool enabled)
{
    m_viewportInteractionRect = rect;
    m_viewportInteractionEnabled = enabled && rect.w > 0.0f && rect.h > 0.0f;
    if (!m_viewportInteractionEnabled)
    {
        m_mouseLookActive = false;
        m_mousePanActive = false;
        m_mouseWheelDelta = 0.0f;
        m_hasMouseLookAnchor = false;
        m_shouldRestoreMouseLookAnchor = false;
    }
}

bool InputState::IsKeyDown(KeyCode key) const
{
    return IsValidKeyCode(key) ? m_keyDown[ToKeyIndex(key)] : false;
}

float InputState::GetGamepadAxis(GamepadAxis axis, uint32_t playerIndex) const
{
    return IsValidGamepadPlayerIndex(playerIndex) && IsValidGamepadAxis(axis)
               ? m_gamepads[playerIndex].axisValues[ToGamepadAxisIndex(axis)]
               : 0.0f;
}

bool InputState::IsGamepadButtonDown(GamepadButton button, uint32_t playerIndex) const
{
    const int buttonIndex = static_cast<int>(button);
    return IsValidGamepadPlayerIndex(playerIndex) && buttonIndex >= 0 && buttonIndex < static_cast<int>(kGamepadButtonCount) &&
           m_gamepads[playerIndex].buttonDown[static_cast<size_t>(buttonIndex)];
}

SDL_GamepadType InputState::GetGamepadType(uint32_t playerIndex) const
{
    return IsValidGamepadPlayerIndex(playerIndex) && m_gamepads[playerIndex].connected ? m_gamepads[playerIndex].type
                                                                                       : SDL_GAMEPAD_TYPE_UNKNOWN;
}

void InputState::SetGamepadFeedback(uint32_t playerIndex, const GamepadFeedback& feedback)
{
    if (IsValidGamepadPlayerIndex(playerIndex) && m_gamepads[playerIndex].connected)
    {
        SendPlatformGamepadFeedback(playerIndex, feedback);
    }
}

void InputState::ClearGamepadFeedback()
{
    for (uint32_t playerIndex = 0; playerIndex < kMaxGamepads; ++playerIndex)
    {
        SetGamepadFeedback(playerIndex, GamepadFeedback{});
    }
}

int InputState::GetFirstConnectedGamepadIndex() const
{
    for (uint32_t playerIndex = 0; playerIndex < kMaxGamepads; ++playerIndex)
    {
        if (m_gamepads[playerIndex].connected)
        {
            return static_cast<int>(playerIndex);
        }
    }

    return -1;
}

bool InputState::IsMouseLookActive() const
{
    return m_mouseLookActive;
}

bool InputState::IsMousePanActive() const
{
    return m_mousePanActive;
}

bool InputState::WantsRelativeMouseMode() const
{
    return m_mouseLookActive || m_mousePanActive;
}

float InputState::GetMouseDeltaX() const
{
    return m_mouseDeltaX;
}

float InputState::GetMouseDeltaY() const
{
    return m_mouseDeltaY;
}

float InputState::GetMouseWheelDelta() const
{
    return m_mouseWheelDelta;
}

bool InputState::ConsumeViewportRightClick()
{
    const bool clicked = m_viewportRightClicked;
    m_viewportRightClicked = false;
    return clicked;
}

bool InputState::ShouldRestoreMouseLookAnchor() const
{
    return !m_mouseLookActive && m_hasMouseLookAnchor && m_shouldRestoreMouseLookAnchor;
}

void InputState::ConsumeMouseLookAnchor(int& x, int& y)
{
    x = m_mouseLookAnchorX;
    y = m_mouseLookAnchorY;
    m_hasMouseLookAnchor = false;
    m_shouldRestoreMouseLookAnchor = false;
}

KeyCode InputState::FromScancode(SDL_Scancode scancode)
{
    return KeyCode{scancode};
}

std::string InputState::GetKeyName(KeyCode key)
{
    const SDL_Scancode scancode = static_cast<SDL_Scancode>(key.value);
    const char* const keyName = SDL_GetScancodeName(scancode);
    if (keyName != nullptr && keyName[0] != '\0')
    {
        return keyName;
    }

    std::ostringstream stream;
    stream << "Scancode(" << key.value << ")";
    return stream.str();
}

std::string InputState::GetGamepadButtonName(GamepadButton button)
{
    switch (button)
    {
    case GamepadButton::South:
        return "A";
    case GamepadButton::East:
        return "B";
    case GamepadButton::West:
        return "X";
    case GamepadButton::North:
        return "Y";
    case GamepadButton::Back:
        return "Back";
    case GamepadButton::Guide:
        return "Guide";
    case GamepadButton::Start:
        return "Start";
    case GamepadButton::LeftStick:
        return "Left Stick";
    case GamepadButton::RightStick:
        return "Right Stick";
    case GamepadButton::LeftShoulder:
        return "Left Shoulder";
    case GamepadButton::RightShoulder:
        return "Right Shoulder";
    case GamepadButton::DpadUp:
        return "DPad Up";
    case GamepadButton::DpadDown:
        return "DPad Down";
    case GamepadButton::DpadLeft:
        return "DPad Left";
    case GamepadButton::DpadRight:
        return "DPad Right";
    case GamepadButton::Misc1:
        return "Misc 1";
    case GamepadButton::RightPaddle1:
        return "Right Paddle 1";
    case GamepadButton::LeftPaddle1:
        return "Left Paddle 1";
    case GamepadButton::RightPaddle2:
        return "Right Paddle 2";
    case GamepadButton::LeftPaddle2:
        return "Left Paddle 2";
    case GamepadButton::Touchpad:
        return "Touchpad";
    case GamepadButton::Misc2:
        return "Misc 2";
    case GamepadButton::Misc3:
        return "Misc 3";
    case GamepadButton::Misc4:
        return "Misc 4";
    case GamepadButton::Misc5:
        return "Misc 5";
    case GamepadButton::Misc6:
        return "Misc 6";
    default:
        return "Invalid Button";
    }
}

std::string InputState::GetGamepadAxisName(GamepadAxis axis)
{
    switch (axis)
    {
    case GamepadAxis::LeftX:
        return "Left Stick X";
    case GamepadAxis::LeftY:
        return "Left Stick Y";
    case GamepadAxis::RightX:
        return "Right Stick X";
    case GamepadAxis::RightY:
        return "Right Stick Y";
    case GamepadAxis::LeftTrigger:
        return "Left Trigger";
    case GamepadAxis::RightTrigger:
        return "Right Trigger";
    default:
        return "Invalid Axis";
    }
}

size_t InputState::ToKeyIndex(KeyCode key)
{
    return static_cast<size_t>(key.value);
}

size_t InputState::ToGamepadAxisIndex(GamepadAxis axis)
{
    return static_cast<size_t>(static_cast<int>(axis));
}

bool InputState::IsValidKeyCode(KeyCode key) const
{
    return key.value < kKeyboardKeyCount;
}

bool InputState::IsValidGamepadPlayerIndex(uint32_t playerIndex) const
{
    return playerIndex < kMaxGamepads;
}

bool InputState::IsValidGamepadAxis(GamepadAxis axis) const
{
    const int axisIndex = static_cast<int>(axis);
    return axisIndex >= 0 && axisIndex < static_cast<int>(kGamepadAxisCount);
}

bool InputState::IsViewportInteractionPoint(float x, float y) const
{
    if (!m_viewportInteractionEnabled)
    {
        return false;
    }

    return x >= m_viewportInteractionRect.x &&
           y >= m_viewportInteractionRect.y &&
           x <= (m_viewportInteractionRect.x + m_viewportInteractionRect.w) &&
           y <= (m_viewportInteractionRect.y + m_viewportInteractionRect.h);
}

void InputState::PollGamepads()
{
    const Uint64 timestampNs = SDL_GetTicksNS();
    const std::array<PolledGamepadState, 4> polledGamepads = PollPlatformGamepads();

    for (uint32_t playerIndex = 0; playerIndex < kMaxGamepads && playerIndex < polledGamepads.size(); ++playerIndex)
    {
        GamepadState& gamepad = m_gamepads[playerIndex];
        const PolledGamepadState& polledGamepad = polledGamepads[playerIndex];
        if (!polledGamepad.connected)
        {
            if (!gamepad.connected)
            {
                continue;
            }

            gamepad = GamepadState{};

            LogGamepadConnectionEvent(timestampNs, playerIndex, "disconnected");
            continue;
        }

        const bool wasConnected = gamepad.connected;
        gamepad.type = polledGamepad.type;
        const std::array<float, kGamepadAxisCount>& updatedAxes = polledGamepad.axisValues;

        if (!wasConnected)
        {
            gamepad.connected = true;
            gamepad.packetNumber = polledGamepad.packetNumber;
            gamepad.buttonDown = polledGamepad.buttonDown;
            gamepad.axisValues = updatedAxes;

            LogGamepadConnectionEvent(timestampNs, playerIndex, "connected");
            continue;
        }

        if (gamepad.packetNumber == polledGamepad.packetNumber)
        {
            continue;
        }

        gamepad.packetNumber = polledGamepad.packetNumber;

        for (size_t buttonIndex = 0; buttonIndex < kGamepadButtonCount; ++buttonIndex)
        {
            const GamepadButton button = static_cast<GamepadButton>(static_cast<int>(buttonIndex));
            const bool isDown = polledGamepad.buttonDown[buttonIndex];
            const bool wasDown = gamepad.buttonDown[buttonIndex];
            if (isDown == wasDown)
            {
                continue;
            }

            gamepad.buttonDown[buttonIndex] = isDown;
            LogGamepadButtonEvent(timestampNs, playerIndex, button, isDown ? "pressed" : "released");
        }

        for (size_t axisIndex = 0; axisIndex < kGamepadAxisCount; ++axisIndex)
        {
            const float previousValue = gamepad.axisValues[axisIndex];
            const float currentValue = updatedAxes[axisIndex];
            if (std::abs(currentValue - previousValue) <= kGamepadAxisChangeEpsilon)
            {
                continue;
            }

            gamepad.axisValues[axisIndex] = currentValue;

            if (ShouldLogAxisChange(previousValue, currentValue))
            {
                LogGamepadAxisEvent(
                    timestampNs,
                    playerIndex,
                    static_cast<GamepadAxis>(static_cast<int>(axisIndex)),
                    currentValue);
            }
        }
    }
}

void InputState::EnsureTimestampBaseInitialized()
{
    if (m_hasTimestampBase)
    {
        return;
    }

    const Uint64 currentTicksNs = SDL_GetTicksNS();
    m_wallClockAtSdlTickZero = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::system_clock::now() - std::chrono::nanoseconds(currentTicksNs));
    m_hasTimestampBase = true;
}

std::string InputState::FormatEventTimestamp(Uint64 timestampNs) const
{
    if (!m_hasTimestampBase)
    {
        return "time-unavailable";
    }

    const auto timePoint = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        m_wallClockAtSdlTickZero + std::chrono::nanoseconds(timestampNs));
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(timePoint.time_since_epoch()) %
        std::chrono::seconds(1);
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(timePoint);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &timeValue);
#else
    localtime_r(&timeValue, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S")
           << '.'
           << std::setfill('0')
           << std::setw(3)
           << milliseconds.count();
    return stream.str();
}

void InputState::LogKeyboardEvent(Uint64 timestampNs, KeyCode key, const char* action) const
{
    LOG_INPUT_INFO(
        "[input][{}][{:.3f} ms] keyboard {} {}",
        FormatEventTimestamp(timestampNs),
        static_cast<double>(timestampNs) / 1'000'000.0,
        action,
        GetKeyName(key));
}

void InputState::LogGamepadConnectionEvent(Uint64 timestampNs, uint32_t playerIndex, const char* action) const
{
    LOG_INPUT_INFO(
        "[input][{}][{:.3f} ms] gamepad[{}] {}",
        FormatEventTimestamp(timestampNs),
        static_cast<double>(timestampNs) / 1'000'000.0,
        playerIndex,
        action);
}

void InputState::LogGamepadButtonEvent(
    Uint64 timestampNs,
    uint32_t playerIndex,
    GamepadButton button,
    const char* action) const
{
    LOG_INPUT_INFO(
        "[input][{}][{:.3f} ms] gamepad[{}] {} {}",
        FormatEventTimestamp(timestampNs),
        static_cast<double>(timestampNs) / 1'000'000.0,
        playerIndex,
        action,
        GetGamepadButtonName(button));
}

void InputState::LogGamepadAxisEvent(Uint64 timestampNs, uint32_t playerIndex, GamepadAxis axis, float value) const
{
    LOG_INPUT_INFO(
        "[input][{}][{:.3f} ms] gamepad[{}] axis {} = {:.3f}",
        FormatEventTimestamp(timestampNs),
        static_cast<double>(timestampNs) / 1'000'000.0,
        playerIndex,
        GetGamepadAxisName(axis),
        value);
}
}
