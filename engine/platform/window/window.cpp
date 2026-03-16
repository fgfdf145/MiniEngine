#include "window.h"

#include <log/log.h>

#include <stdexcept>

Window::Window(int width, int height, const char* title, WindowGraphicsApi graphicsApi)
    : m_width(width),
      m_height(height),
      m_title(title),
      m_graphicsApi(graphicsApi)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

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

void Window::Recreate(WindowGraphicsApi graphicsApi)
{
    m_graphicsApi = graphicsApi;
    if (m_window)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    CreateNativeWindow();
    m_running = true;
}

void Window::CreateNativeWindow()
{
    SDL_GL_ResetAttributes();
    if (m_graphicsApi == WindowGraphicsApi::OpenGL)
    {
        if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
            !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) ||
            !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) ||
            !SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8))
        {
            throw std::runtime_error(std::string("SDL_GL_SetAttribute failed: ") + SDL_GetError());
        }
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (m_graphicsApi == WindowGraphicsApi::OpenGL)
    {
        flags |= SDL_WINDOW_OPENGL;
    }

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
        m_graphicsApi == WindowGraphicsApi::Vulkan ? "Vulkan" : "OpenGL"
    );
}
