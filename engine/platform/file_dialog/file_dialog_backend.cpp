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
#elif defined(__APPLE__)
std::string TrimAsciiWhitespace(std::string value)
{
    const auto notWhitespace = [](unsigned char character)
    {
        return !std::isspace(character);
    };

    const auto begin = std::find_if(value.begin(), value.end(), notWhitespace);
    const auto end = std::find_if(value.rbegin(), value.rend(), notWhitespace).base();
    if (begin >= end)
    {
        return {};
    }

    return std::string(begin, end);
}

std::string EscapeAppleScriptString(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value)
    {
        if (character == '\\' || character == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }

    return escaped;
}

std::string EscapeShellSingleQuotedString(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value)
    {
        if (character == '\'')
        {
            escaped += "'\\''";
        }
        else
        {
            escaped.push_back(character);
        }
    }

    return escaped;
}

std::string QuoteAppleScriptString(std::string_view value)
{
    return '"' + EscapeAppleScriptString(value) + '"';
}

std::string BuildAppleScriptTypeList(std::initializer_list<const char*> fileTypes)
{
    std::string result = "{";
    bool first = true;

    for (const char* fileType : fileTypes)
    {
        if (!first)
        {
            result += ", ";
        }

        result += QuoteAppleScriptString(fileType);
        first = false;
    }

    result += '}';
    return result;
}

std::optional<std::string> RunAppleScript(const std::string& script)
{
    const std::string command =
        "/usr/bin/osascript -e '" + EscapeShellSingleQuotedString(script) + "'";

    FILE* const pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return std::nullopt;
    }

    std::array<char, 512> buffer{};
    std::string output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        output += buffer.data();
    }

    const int exitCode = pclose(pipe);
    if (exitCode != 0)
    {
        return std::nullopt;
    }

    const std::string trimmedOutput = TrimAsciiWhitespace(output);
    return trimmedOutput.empty() ? std::nullopt : std::optional<std::string>(trimmedOutput);
}

std::optional<std::string> ShowMacOpenFileDialog(
    std::string_view prompt,
    std::initializer_list<const char*> fileTypes
)
{
    std::string script = "POSIX path of (choose file with prompt " + QuoteAppleScriptString(prompt);
    if (fileTypes.size() > 0)
    {
        script += " of type " + BuildAppleScriptTypeList(fileTypes);
    }
    script += ')';
    return RunAppleScript(script);
}

std::optional<std::string> ShowMacSaveFileDialog(std::string_view prompt, std::string_view defaultName)
{
    const std::string script =
        "POSIX path of (choose file name with prompt " + QuoteAppleScriptString(prompt) +
        " default name " + QuoteAppleScriptString(defaultName) + ')';
    return RunAppleScript(script);
}
#endif
}

namespace platform::file_dialog
{
bool SupportsNativeFileDialogs()
{
#if defined(_WIN32) || defined(__APPLE__)
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
#elif defined(__APPLE__)
    switch (type)
    {
    case FileDialogType::OpenModel:
        return ShowMacOpenFileDialog("Import glTF Model", { "gltf", "glb" });
    case FileDialogType::OpenTexture:
        return ShowMacOpenFileDialog("Choose Texture", { "public.image" });
    case FileDialogType::OpenScene:
        return ShowMacOpenFileDialog("Open Scene", { "yaml", "yml" });
    case FileDialogType::SaveScene:
    {
        std::optional<std::string> result = ShowMacSaveFileDialog("Save Scene", "scene.yaml");
        if (!result.has_value())
        {
            return std::nullopt;
        }

        if (!result->ends_with(".yaml") && !result->ends_with(".yml"))
        {
            result->append(".yaml");
        }

        return result;
    }
    }

    return std::nullopt;
#else
    static_cast<void>(type);
    return std::nullopt;
#endif
}
}
