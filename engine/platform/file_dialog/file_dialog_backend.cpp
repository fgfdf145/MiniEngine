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
#endif

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
        nullptr
    );
    std::string utf8(static_cast<size_t>(bufferSize), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.c_str(),
        static_cast<int>(wide.size()),
        utf8.data(),
        bufferSize,
        nullptr,
        nullptr
    );
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
#endif
}

namespace platform::file_dialog
{
bool SupportsNativeFileDialogs()
{
#if defined(_WIN32)
    return true;
#else
    return false;
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
            L"Texture Files\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.gif;*.hdr;*.dds\0"
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
    static_cast<void>(type);
    return std::nullopt;
#endif
}
}
