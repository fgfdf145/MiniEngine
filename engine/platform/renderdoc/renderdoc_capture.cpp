#include "renderdoc_capture.h"

#include <engine/core/log/log.h>

#include <renderdoc/renderdoc_app.h>

#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace me::platform::renderdoc
{

namespace
{
RENDERDOC_API_1_6_0* g_api = nullptr;

void* FindGetApi(std::string& error)
{
#ifdef _WIN32
    // Launched from the RenderDoc UI: it is in the process already.
    HMODULE module = GetModuleHandleW(L"renderdoc.dll");
    if (module == nullptr)
    {
        std::filesystem::path dll = "C:/Program Files/RenderDoc/renderdoc.dll";
        if (const char* overridePath = std::getenv("MINIENGINE_RENDERDOC_DLL"); overridePath != nullptr && overridePath[0] != 0)
        {
            dll = overridePath;
        }
        module = LoadLibraryW(dll.wstring().c_str());
        if (module == nullptr)
        {
            error = "cannot load '" + dll.string() + "' (is RenderDoc installed? MINIENGINE_RENDERDOC_DLL names another copy)";
            return nullptr;
        }
    }
    return reinterpret_cast<void*>(GetProcAddress(module, "RENDERDOC_GetAPI"));
#else
    void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr)
    {
        const char* overridePath = std::getenv("MINIENGINE_RENDERDOC_DLL");
        module = dlopen(overridePath != nullptr ? overridePath : "librenderdoc.so", RTLD_NOW);
    }
    if (module == nullptr)
    {
        error = "cannot load librenderdoc.so";
        return nullptr;
    }
    return dlsym(module, "RENDERDOC_GetAPI");
#endif
}
}

bool Load(const std::filesystem::path& captureFolder, std::string& error)
{
    if (g_api != nullptr)
    {
        return true;
    }
    auto* getApi = reinterpret_cast<pRENDERDOC_GetAPI>(FindGetApi(error));
    if (getApi == nullptr)
    {
        if (error.empty())
        {
            error = "renderdoc.dll has no RENDERDOC_GetAPI";
        }
        return false;
    }
    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1 || g_api == nullptr)
    {
        g_api = nullptr;
        error = "this RenderDoc is older than API 1.6";
        return false;
    }
    std::error_code ignored;
    std::filesystem::create_directories(captureFolder, ignored);
    // RenderDoc appends the frame number and .rdc to the template.
    g_api->SetCaptureFilePathTemplate((captureFolder / "miniengine").string().c_str());
    g_api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    // The engine asks for captures itself; F12 and Print Screen stay the game's.
    g_api->SetCaptureKeys(nullptr, 0);
    int major = 0;
    int minor = 0;
    int patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);
    LOG_INFO("RenderDoc {}.{}.{} loaded; captures go to '{}'", major, minor, patch, captureFolder.string());
    return true;
}

bool IsLoaded()
{
    return g_api != nullptr;
}

void TriggerCapture(uint32_t frames)
{
    if (g_api != nullptr)
    {
        g_api->TriggerMultiFrameCapture(frames == 0 ? 1 : frames);
    }
}

bool IsCapturing()
{
    return g_api != nullptr && g_api->IsFrameCapturing() != 0;
}

std::vector<CaptureFile> Captures()
{
    std::vector<CaptureFile> captures;
    if (g_api == nullptr)
    {
        return captures;
    }
    const uint32_t count = g_api->GetNumCaptures();
    for (uint32_t index = 0; index < count; ++index)
    {
        uint32_t length = 0;
        uint64_t timestamp = 0;
        if (g_api->GetCapture(index, nullptr, &length, &timestamp) == 0 || length == 0)
        {
            continue;
        }
        std::string path(length, '\0');
        g_api->GetCapture(index, path.data(), &length, &timestamp);
        path.resize(std::char_traits<char>::length(path.c_str()));
        captures.push_back({std::filesystem::path(path), timestamp});
    }
    return captures;
}

bool OpenInUi(const std::filesystem::path& capture, bool connect)
{
    if (g_api == nullptr)
    {
        return false;
    }
    const std::string path = capture.string();
    return g_api->LaunchReplayUI(connect ? 1 : 0, path.empty() ? nullptr : path.c_str()) != 0;
}
}
