#pragma once
#include <SDL3/SDL.h>

class Window
{
public:
    Window(int width, int height, const char* title);
    ~Window();

    bool ShouldClose() const;
    void PollEvents();

    SDL_Window* GetSDLWindow() const;

    SDL_Window* m_window = nullptr;
private:
    bool m_running = true;
};
