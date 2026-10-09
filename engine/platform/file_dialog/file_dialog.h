#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace me
{

enum class FileDialogType
{
    OpenModel,
    OpenTexture,
    OpenScene,
    SaveScene,
    // A folder, not a file (Photo Mode's save folder).
    OpenFolder,
};

bool SupportsNativeFileDialogs();
std::optional<std::string> ShowFileDialog(FileDialogType type);

// Opens a folder in the system's file browser (Explorer, Finder, the desktop's), or a file in the
// application that opens its type; false (with the reason logged) when the system would not.
bool OpenInFileBrowser(const std::filesystem::path& path);
}
