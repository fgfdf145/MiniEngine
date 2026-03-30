#include "window_platform.h"

#include <log/log.h>

#include <SDL3/SDL.h>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace platform::window
{
void ApplyPlatformWindowHints()
{
#if defined(_WIN32)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "windows");

    char systemDirectory[MAX_PATH + 1] = {};
    const UINT systemDirectoryLength = GetSystemDirectoryA(systemDirectory, MAX_PATH);
    if (systemDirectoryLength > 0 && systemDirectoryLength <= MAX_PATH)
    {
        std::string vulkanLoaderPath(systemDirectory, systemDirectoryLength);
        vulkanLoaderPath += "\\vulkan-1.dll";
        if (SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, vulkanLoaderPath.c_str()))
        {
            LOG_INFO("SDL Vulkan loader hint: {}", vulkanLoaderPath);
        }
    }
#endif
}
}
