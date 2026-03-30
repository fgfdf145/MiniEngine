#pragma once

#include <optional>
#include <string>

enum class FileDialogType
{
    OpenModel,
    OpenTexture,
    OpenScene,
    SaveScene,
};

bool SupportsNativeFileDialogs();
std::optional<std::string> ShowFileDialog(FileDialogType type);

std::optional<std::string> OpenModelFileDialog();
std::optional<std::string> OpenTextureFileDialog();
std::optional<std::string> OpenSceneFileDialog();
std::optional<std::string> SaveSceneFileDialog();
