#include "window.h"
#include "window_platform.h"

#include <log/log.h>

#include <algorithm>
#include <stdexcept>

namespace
{
void ClampWindowSizeToPrimaryDisplay(int& width, int& height)
{
    const SDL_DisplayID primaryDisplay = SDL_GetPrimaryDisplay();
    if (primaryDisplay == 0)
    {
        return;
    }

    SDL_Rect usableBounds{};
    if (!SDL_GetDisplayUsableBounds(primaryDisplay, &usableBounds) ||
        usableBounds.w <= 0 ||
        usableBounds.h <= 0)
    {
        return;
    }

    constexpr int kWindowMargin = 64;
    const int maxWidth = std::max(usableBounds.w - kWindowMargin, 1);
    const int maxHeight = std::max(usableBounds.h - kWindowMargin, 1);
    const int minWidth = std::min(960, maxWidth);
    const int minHeight = std::min(720, maxHeight);
    width = std::clamp(width, minWidth, maxWidth);
    height = std::clamp(height, minHeight, maxHeight);
}
}

Window::Window(int width, int height, const char* title, RenderBackendType backendType)
    : m_width(width),
      m_height(height),
      m_backendType(backendType),
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

RenderBackendType Window::GetBackendType() const
{
    return m_backendType;
}

void Window::CreateNativeWindow()
{
    ClampWindowSizeToPrimaryDisplay(m_width, m_height);

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    switch (m_backendType)
    {
    case RenderBackendType::Vulkan:
        flags |= SDL_WINDOW_VULKAN;
        break;
    default:
        throw std::runtime_error("Unsupported window backend type");
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

    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    LOG_INFO(
        "SDL window created: {}x{} ({})",
        m_width,
        m_height,
        ToString(m_backendType)
    );
}
