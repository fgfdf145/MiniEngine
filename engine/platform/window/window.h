#pragma once
#include <SDL3/SDL.h>

#include <functional>
#include <string>

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
    void CreateNativeWindow();

    bool m_running = true;
    int m_width = 0;
    int m_height = 0;
    std::string m_title;
};
