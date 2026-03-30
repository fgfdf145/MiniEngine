#include "window.h"
#include "window_platform.h"

#include <log/log.h>

#include <stdexcept>

Window::Window(int width, int height, const char* title)
    : m_width(width),
      m_height(height),
      m_title(title)
{
    if (!SDL_SetAppMetadata("MiniEngine", "0.1.0", "com.miniengine.editor"))
    {
        LOG_WARN("SDL_SetAppMetadata failed: {}", SDL_GetError());
    }

    platform::window::ApplyPlatformWindowHints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    LOG_INFO("SDL video driver: {}", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "<unknown>");
    CreateNativeWindow();
}

Window::~Window()
{
    if (m_window)
    {
        SDL_DestroyWindow(m_window);
    }

    SDL_Quit();
}

bool Window::ShouldClose() const
{
    return !m_running;
}

void Window::PollEvents(const std::function<void(const SDL_Event&)>& eventHandler)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        if (eventHandler)
        {
            eventHandler(event);
        }

        if (event.type == SDL_EVENT_QUIT)
        {
            m_running = false;
        }
    }
}

SDL_Window* Window::GetSDLWindow() const
{
    return m_window;
}

void Window::CreateNativeWindow()
{
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_VULKAN |
        SDL_WINDOW_HIGH_PIXEL_DENSITY;

    m_window = SDL_CreateWindow(
        m_title.c_str(),
        m_width,
        m_height,
        flags
    );

    if (!m_window)
    {
        SDL_Quit();
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    LOG_INFO(
        "SDL window created: {}x{} ({})",
        m_width,
        m_height,
        "Vulkan"
    );
}
