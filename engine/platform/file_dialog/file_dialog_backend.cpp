#include "file_dialog_backend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <engine/core/log/log.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#endif

namespace me
{

namespace
{
#ifdef _WIN32
std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }

    const int bufferSize = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.c_str(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    std::string utf8(static_cast<size_t>(bufferSize), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.c_str(),
        static_cast<int>(wide.size()),
        utf8.data(),
        bufferSize,
        nullptr,
        nullptr);
    return utf8;
}

std::optional<std::string> ShowWindowsFileDialog(OPENFILENAMEW& dialog, bool saveDialog)
{
    const BOOL result = saveDialog ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!result)
    {
        return std::nullopt;
    }

    return WideToUtf8(dialog.lpstrFile);
}
#else
// Cleared the first time SDL reports that it cannot show a dialog (e.g. no portal or zenity on
// Linux), so the editor falls back to typing the path instead of offering a button that never works.
std::atomic<bool> g_sdlDialogsAvailable{true};

// SDL's dialogs report through a callback. On macOS it runs before SDL_Show*FileDialog returns; on
// Linux it can come later and from another thread, so the result is handed over under a mutex.
struct SdlDialogResult
{
    std::mutex mutex;
    bool done = false;
    std::optional<std::string> path;
};

void SDLCALL OnSdlDialogFinished(void* userdata, const char* const* filelist, int filter)
{
    static_cast<void>(filter);
    auto* result = static_cast<SdlDialogResult*>(userdata);
    std::lock_guard lock(result->mutex);
    if (filelist == nullptr)
    {
        LOG_WARN("Native file dialog unavailable: {}", SDL_GetError());
        g_sdlDialogsAvailable = false;
    }
    else if (filelist[0] != nullptr)
    {
        result->path = std::string(filelist[0]);
    }
    result->done = true;
}

std::optional<std::string> ShowSdlFileDialog(FileDialogType type)
{
    static constexpr SDL_DialogFileFilter kModelFilters[] = {
        {"glTF Files", "gltf;glb"},
        {"All Files", "*"},
    };
    static constexpr SDL_DialogFileFilter kTextureFilters[] = {
        {"Texture Files", "png;jpg;jpeg;tga;bmp;gif;hdr;exr;dds"},
        {"All Files", "*"},
    };
    static constexpr SDL_DialogFileFilter kSceneFilters[] = {
        {"Scene Files", "yaml;yml"},
        {"All Files", "*"},
    };

    SdlDialogResult result;
    // No parent window: on macOS SDL then runs the panel modally and returns with the answer, which
    // keeps this call synchronous like the Windows dialog.
    switch (type)
    {
    case FileDialogType::OpenModel:
        SDL_ShowOpenFileDialog(OnSdlDialogFinished, &result, nullptr, kModelFilters, 2, nullptr, false);
        break;
    case FileDialogType::OpenTexture:
        SDL_ShowOpenFileDialog(OnSdlDialogFinished, &result, nullptr, kTextureFilters, 2, nullptr, false);
        break;
    case FileDialogType::OpenScene:
        SDL_ShowOpenFileDialog(OnSdlDialogFinished, &result, nullptr, kSceneFilters, 2, nullptr, false);
        break;
    case FileDialogType::SaveScene:
        SDL_ShowSaveFileDialog(OnSdlDialogFinished, &result, nullptr, kSceneFilters, 2, nullptr);
        break;
    }

    // Backends that answer asynchronously need the event loop pumped to make progress.
    for (;;)
    {
        {
            std::lock_guard lock(result.mutex);
            if (result.done)
            {
                break;
            }
        }
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    std::lock_guard lock(result.mutex);
    // Match the Windows dialog's default extension.
    if (type == FileDialogType::SaveScene && result.path.has_value() &&
        std::filesystem::path(*result.path).extension().empty())
    {
        *result.path += ".yaml";
    }
    return result.path;
}
#endif
}

namespace platform::file_dialog
{
bool SupportsNativeFileDialogs()
{
#if defined(_WIN32)
    return true;
#else
    return g_sdlDialogsAvailable;
#endif
}

std::optional<std::string> ShowFileDialog(FileDialogType type)
{
#if defined(_WIN32)
    std::vector<wchar_t> fileBuffer(32768, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());

    switch (type)
    {
    case FileDialogType::OpenModel:
        dialog.lpstrFilter =
            L"glTF Files\0*.gltf;*.glb\0"
            L"All Files\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"gltf";
        return ShowWindowsFileDialog(dialog, false);
    case FileDialogType::OpenTexture:
        dialog.lpstrFilter =
            L"Texture Files\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.gif;*.hdr;*.exr;*.dds\0"
            L"All Files\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"png";
        return ShowWindowsFileDialog(dialog, false);
    case FileDialogType::OpenScene:
        dialog.lpstrFilter =
            L"Scene Files\0*.yaml;*.yml\0"
            L"All Files\0*.*\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"yaml";
        return ShowWindowsFileDialog(dialog, false);
    case FileDialogType::SaveScene:
        dialog.lpstrFilter =
            L"Scene Files\0*.yaml;*.yml\0"
            L"All Files\0*.*\0";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"yaml";
        return ShowWindowsFileDialog(dialog, true);
    }

    return std::nullopt;
#else
    if (!g_sdlDialogsAvailable)
    {
        return std::nullopt;
    }
    return ShowSdlFileDialog(type);
#endif
}
}
}
