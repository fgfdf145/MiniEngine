#include "window.h"

#include <log/log.h>

Window::Window(int width, int height, const char* title)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        m_running = false;
        return;
    }

    m_window = SDL_CreateWindow(
        title,
        width,
        height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!m_window)
    {
        LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        m_running = false;
        SDL_Quit();
        return;
    }

    LOG_INFO("SDL window created: {}x{}", width, height);
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

void Window::PollEvents()
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
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
