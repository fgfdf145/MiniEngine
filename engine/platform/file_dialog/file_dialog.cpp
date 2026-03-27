#include "file_dialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#include <stdexcept>
#include <vector>

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

std::optional<std::string> ShowFileDialog(OPENFILENAMEW& dialog, bool saveDialog)
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

std::optional<std::string> OpenModelFileDialog()
{
#ifdef _WIN32
    std::vector<wchar_t> fileBuffer(32768, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Model Files\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.stl\0"
        L"All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"obj";

    return ShowFileDialog(dialog, false);
#else
    throw std::runtime_error("OpenModelFileDialog is only implemented on Windows");
#endif
}

std::optional<std::string> OpenTextureFileDialog()
{
#ifdef _WIN32
    std::vector<wchar_t> fileBuffer(32768, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Texture Files\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.gif;*.hdr;*.dds\0"
        L"All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"png";

    return ShowFileDialog(dialog, false);
#else
    throw std::runtime_error("OpenTextureFileDialog is only implemented on Windows");
#endif
}

std::optional<std::string> OpenSceneFileDialog()
{
#ifdef _WIN32
    std::vector<wchar_t> fileBuffer(32768, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Scene Files\0*.yaml;*.yml\0"
        L"All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"yaml";

    return ShowFileDialog(dialog, false);
#else
    throw std::runtime_error("OpenSceneFileDialog is only implemented on Windows");
#endif
}

std::optional<std::string> SaveSceneFileDialog()
{
#ifdef _WIN32
    std::vector<wchar_t> fileBuffer(32768, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Scene Files\0*.yaml;*.yml\0"
        L"All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"yaml";

    return ShowFileDialog(dialog, true);
#else
    throw std::runtime_error("SaveSceneFileDialog is only implemented on Windows");
#endif
}
