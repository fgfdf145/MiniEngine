#include "file_dialog.h"

#include "file_dialog_backend.h"

#include <engine/core/log/log.h>

#include <SDL3/SDL.h>

namespace me
{

bool SupportsNativeFileDialogs()
{
    return platform::file_dialog::SupportsNativeFileDialogs();
}

std::optional<std::string> ShowFileDialog(FileDialogType type)
{
    return platform::file_dialog::ShowFileDialog(type);
}

bool OpenInFileBrowser(const std::filesystem::path& path)
{
    // A file URL: three slashes before a drive letter, two before an absolute POSIX path. Spaces are
    // the one character such paths commonly hold that a URL cannot.
    std::error_code error;
    const std::string absolute = std::filesystem::absolute(path, error).generic_string();
    std::string url = absolute.starts_with('/') ? "file://" : "file:///";
    for (const char character : absolute)
    {
        url += character == ' ' ? std::string("%20") : std::string(1, character);
    }
    if (!SDL_OpenURL(url.c_str()))
    {
        LOG_WARN("Could not open {}: {}", url, SDL_GetError());
        return false;
    }
    return true;
}

}
