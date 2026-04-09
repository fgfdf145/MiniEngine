#pragma once

#include <render_backend_type.h>
#include <SDL3/SDL.h>

#include <functional>
#include <string>

class Window
{
public:
    Window(int width, int height, const char* title, RenderBackendType backendType);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    bool ShouldClose() const;
    void PollEvents(const std::function<void(const SDL_Event&)>& eventHandler = {});

    SDL_Window* GetSDLWindow() const;
    RenderBackendType GetBackendType() const;

private:
    void CreateNativeWindow();

    SDL_Window* m_window = nullptr;
    bool m_running = true;
    int m_width = 0;
    int m_height = 0;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
    std::string m_title;
};
