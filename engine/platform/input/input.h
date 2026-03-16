#pragma once

#include <SDL3/SDL.h>

#include <array>

class InputState
{
public:
    void HandleEvent(const SDL_Event& event);
    void EndFrame();

    bool IsKeyDown(SDL_Scancode scancode) const;
    bool IsMouseLookActive() const;
    bool IsMousePanActive() const;
    bool WantsRelativeMouseMode() const;
    float GetMouseDeltaX() const;
    float GetMouseDeltaY() const;

private:
    std::array<bool, SDL_SCANCODE_COUNT> m_keys{};
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
    bool m_mouseLookActive = false;
    bool m_mousePanActive = false;
};
