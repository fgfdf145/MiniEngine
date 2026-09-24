#pragma once

#include <engine/core/render_backend_type.h>
#include <SDL3/SDL.h>

#include <exception>
#include <functional>
#include <string>

namespace me
{

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
    // While the user drags a window edge, the OS runs its own modal loop and PollEvents does not
    // return until the mouse is released. SDL still delivers live-resize expose events from inside
    // that loop; the handler is called for each one so the caller can draw a frame at the new size.
    // Pass an empty function to remove it.
    void SetLiveResizeHandler(std::function<void()> handler);

    SDL_Window* GetSDLWindow() const;
    RenderBackendType GetBackendType() const;

  private:
    void CreateNativeWindow();
    static bool SDLCALL LiveResizeEventWatch(void* userdata, SDL_Event* event);

    SDL_Window* m_window = nullptr;
    bool m_running = true;
    std::function<void()> m_liveResizeHandler;
    bool m_inLiveResizeHandler = false;
    // An exception cannot unwind through SDL's and the OS's C frames, so one thrown by the handler
    // is held here and rethrown by PollEvents once the event pump has returned.
    std::exception_ptr m_liveResizeError;
    int m_width = 0;
    int m_height = 0;
    RenderBackendType m_backendType = RenderBackendType::Vulkan;
    std::string m_title;
};
}
