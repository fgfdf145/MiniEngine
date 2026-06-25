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
#endif
}
}
