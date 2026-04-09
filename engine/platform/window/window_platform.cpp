#include "window_platform.h"

#include <log/log.h>

#include <SDL3/SDL.h>
#include <filesystem>
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

    std::string vulkanLoaderPath;

    if (const HMODULE loadedVulkanModule = GetModuleHandleA("vulkan-1.dll"); loadedVulkanModule != nullptr)
    {
        char loadedModulePath[MAX_PATH + 1] = {};
        const DWORD loadedModulePathLength = GetModuleFileNameA(loadedVulkanModule, loadedModulePath, MAX_PATH);
        if (loadedModulePathLength > 0 && loadedModulePathLength <= MAX_PATH)
        {
            vulkanLoaderPath.assign(loadedModulePath, loadedModulePathLength);
        }
    }

    if (vulkanLoaderPath.empty())
    {
        char executablePath[MAX_PATH + 1] = {};
        const DWORD executablePathLength = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
        if (executablePathLength > 0 && executablePathLength <= MAX_PATH)
        {
            const std::filesystem::path localVulkanLoaderPath =
                std::filesystem::path(std::string(executablePath, executablePathLength)).parent_path() / "vulkan-1.dll";
            if (std::filesystem::exists(localVulkanLoaderPath))
            {
                vulkanLoaderPath = localVulkanLoaderPath.string();
            }
        }
    }

    if (!vulkanLoaderPath.empty())
    {
        if (SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, vulkanLoaderPath.c_str()))
        {
            LOG_INFO("SDL Vulkan loader hint: {}", vulkanLoaderPath);
        }
    }
    else
    {
        LOG_INFO("SDL Vulkan loader hint: using SDL default discovery");
    }
#elif defined(__APPLE__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "cocoa");

#ifdef MINIENGINE_VULKAN_LOADER_PATH
    constexpr const char* vulkanLoaderPath = MINIENGINE_VULKAN_LOADER_PATH;
    std::error_code errorCode;
    if (std::filesystem::exists(vulkanLoaderPath, errorCode) && !errorCode)
    {
        if (SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, vulkanLoaderPath))
        {
            LOG_INFO("SDL Vulkan loader hint: {}", vulkanLoaderPath);
        }
    }
    else
#endif
    {
        LOG_INFO("SDL Vulkan loader hint: using SDL default discovery");
    }
#endif
}
}
