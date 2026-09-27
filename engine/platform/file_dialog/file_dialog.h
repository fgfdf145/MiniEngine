#pragma once

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
};

bool SupportsNativeFileDialogs();
std::optional<std::string> ShowFileDialog(FileDialogType type);
}
