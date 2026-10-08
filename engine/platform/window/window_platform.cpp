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
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstdlib>
#include <unordered_map>
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

#if defined(_WIN32)
namespace
{
const char* VirtualDesktopRequest()
{
    const char* request = std::getenv("MINIENGINE_VIRTUAL_DESKTOP");
    return request != nullptr && request[0] != '\0' ? request : nullptr;
}

std::string GuidString(const GUID& guid)
{
    wchar_t wide[64] = {};
    StringFromGUID2(guid, wide, 64);
    std::string narrow;
    for (const wchar_t* c = wide; *c != L'\0'; ++c)
    {
        narrow.push_back(static_cast<char>(*c));
    }
    return narrow;
}

bool ParseGuid(const std::string& text, GUID& guid)
{
    std::string braced = text;
    if (braced.front() != '{')
    {
        braced = "{" + braced + "}";
    }
    const std::wstring wide(braced.begin(), braced.end());
    return SUCCEEDED(CLSIDFromString(wide.c_str(), &guid));
}

struct ProcessWindowSearch
{
    DWORD processId = 0;
    IVirtualDesktopManager* manager = nullptr;
    GUID desktop = GUID_NULL;
};

BOOL CALLBACK FindProcessWindowDesktop(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<ProcessWindowSearch*>(param);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
    {
        return TRUE;
    }
    GUID desktop = GUID_NULL;
    if (SUCCEEDED(search->manager->GetWindowDesktopId(hwnd, &desktop)) && desktop != GUID_NULL)
    {
        search->desktop = desktop;
        return FALSE;
    }
    return TRUE;
}

// The desktop of the first visible top-level window owned by this process's parent, grandparent
// and so on: the terminal, IDE or agent app that started the engine.
GUID LauncherDesktop(IVirtualDesktopManager* manager)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return GUID_NULL;
    }
    std::unordered_map<DWORD, DWORD> parentOf;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        parentOf[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
    CloseHandle(snapshot);

    DWORD processId = GetCurrentProcessId();
    // The depth bound also stops a cycle made by a reused parent id.
    for (int depth = 0; depth < 16; ++depth)
    {
        const auto parent = parentOf.find(processId);
        if (parent == parentOf.end() || parent->second == 0)
        {
            break;
        }
        processId = parent->second;
        ProcessWindowSearch search{processId, manager};
        EnumWindows(FindProcessWindowDesktop, reinterpret_cast<LPARAM>(&search));
        if (search.desktop != GUID_NULL)
        {
            return search.desktop;
        }
    }
    return GUID_NULL;
}

void MoveToRequestedDesktop(HWND hwnd, const std::string& request)
{
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IVirtualDesktopManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))))
    {
        LOG_WARN("Virtual desktop: IVirtualDesktopManager is unavailable");
    }
    else
    {
        GUID desktop = GUID_NULL;
        if (request == "launcher")
        {
            desktop = LauncherDesktop(manager);
        }
        else if (!ParseGuid(request, desktop))
        {
            LOG_WARN("Virtual desktop: '{}' is neither 'launcher' nor a desktop GUID", request);
        }

        if (desktop == GUID_NULL)
        {
            LOG_WARN("Virtual desktop: no desktop found for '{}'; the window opens on the current one", request);
        }
        else if (const HRESULT moved = manager->MoveWindowToDesktop(hwnd, desktop); FAILED(moved))
        {
            LOG_WARN("Virtual desktop: moving the window to {} failed (0x{:08X})",
                GuidString(desktop),
                static_cast<unsigned>(moved));
        }
        else
        {
            LOG_INFO("Virtual desktop: window opened on {} ({})", GuidString(desktop), request);
        }
        manager->Release();
    }
    if (SUCCEEDED(comInit))
    {
        CoUninitialize();
    }
}
}
#endif

bool HasVirtualDesktopRequest()
{
#if defined(_WIN32)
    return VirtualDesktopRequest() != nullptr;
#else
    return false;
#endif
}

void ShowOnRequestedVirtualDesktop(SDL_Window* window, bool keepHidden)
{
#if defined(_WIN32)
    const char* request = VirtualDesktopRequest();
    auto* hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (request != nullptr && hwnd != nullptr)
    {
        MoveToRequestedDesktop(hwnd, request);
    }
#endif
    if (!keepHidden)
    {
        // Shown without activation: activating a window on another desktop switches the user there.
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_ShowWindow(window);
    }
}
}
}
