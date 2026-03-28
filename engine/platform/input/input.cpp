#include "input.h"

void InputState::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat)
        {
            m_keys[event.key.scancode] = true;
        }
        break;
    case SDL_EVENT_KEY_UP:
        m_keys[event.key.scancode] = false;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        m_mouseDeltaX += event.motion.xrel;
        m_mouseDeltaY += event.motion.yrel;
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
        }
        if (event.button.button == SDL_BUTTON_MIDDLE &&
            IsViewportInteractionPoint(event.button.x, event.button.y))
        {
            m_mousePanActive = true;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT)
        {
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

void InputState::EndFrame()
{
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
}

void InputState::SetViewportInteractionRegion(const SDL_FRect& rect, bool enabled)
{
    m_viewportInteractionRect = rect;
    m_viewportInteractionEnabled = enabled && rect.w > 0.0f && rect.h > 0.0f;
    if (!m_viewportInteractionEnabled)
    {
        m_mouseLookActive = false;
        m_mousePanActive = false;
        m_hasMouseLookAnchor = false;
        m_shouldRestoreMouseLookAnchor = false;
    }
}

bool InputState::IsKeyDown(SDL_Scancode scancode) const
{
    return m_keys[scancode];
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

bool InputState::IsViewportInteractionPoint(float x, float y) const
{
    if (!m_viewportInteractionEnabled)
    {
        return false;
    }

    return
        x >= m_viewportInteractionRect.x &&
        y >= m_viewportInteractionRect.y &&
        x <= (m_viewportInteractionRect.x + m_viewportInteractionRect.w) &&
        y <= (m_viewportInteractionRect.y + m_viewportInteractionRect.h);
}
