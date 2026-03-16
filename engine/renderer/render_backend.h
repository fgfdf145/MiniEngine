#pragma once

#include <SDL3/SDL_events.h>

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;
    virtual void HandleEvent(const SDL_Event& event) = 0;
    virtual void DrawFrame() = 0;
};
