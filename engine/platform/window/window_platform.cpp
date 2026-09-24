#include "window_platform.h"

#include <engine/core/log/log.h>

#include <SDL3/SDL.h>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
#include <dlfcn.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#endif

namespace me
{

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
    // SDL dlopens "libvulkan.dylib" by leaf name, which never searches the app's rpath, so point it
    // at the loader this process already linked against.
    Dl_info loaderInfo = {};
    if (dladdr(reinterpret_cast<const void*>(&vkGetInstanceProcAddr), &loaderInfo) != 0 &&
        loaderInfo.dli_fname != nullptr)
    {
        if (SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, loaderInfo.dli_fname))
        {
            LOG_INFO("SDL Vulkan loader hint: {}", loaderInfo.dli_fname);
        }
    }
    else
    {
        LOG_INFO("SDL Vulkan loader hint: using SDL default discovery");
    }

#ifdef MINIENGINE_MOLTENVK_ICD_PATH
    // The loader does not search Homebrew's prefix for drivers. Respect any driver selection the
    // user already made.
    if (getenv("VK_DRIVER_FILES") == nullptr && getenv("VK_ICD_FILENAMES") == nullptr &&
        getenv("VK_ADD_DRIVER_FILES") == nullptr)
    {
        if (std::filesystem::exists(MINIENGINE_MOLTENVK_ICD_PATH) &&
            setenv("VK_DRIVER_FILES", MINIENGINE_MOLTENVK_ICD_PATH, 0) == 0)
        {
            LOG_INFO("Vulkan driver: {}", MINIENGINE_MOLTENVK_ICD_PATH);
        }
    }
#endif
#endif
}
}
}
