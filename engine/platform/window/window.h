#pragma once
#include <SDL3/SDL.h>

#include <functional>

class Window
{
public:
    Window(int width, int height, const char* title);
    ~Window();

    bool ShouldClose() const;
    void PollEvents(const std::function<void(const SDL_Event&)>& eventHandler = {});

    SDL_Window* GetSDLWindow() const;

    SDL_Window* m_window = nullptr;
private:
    bool m_running = true;
};
