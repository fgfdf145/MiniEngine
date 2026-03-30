#include "file_dialog.h"

#include "file_dialog_backend.h"

bool SupportsNativeFileDialogs()
{
    return platform::file_dialog::SupportsNativeFileDialogs();
}

std::optional<std::string> ShowFileDialog(FileDialogType type)
{
    return platform::file_dialog::ShowFileDialog(type);
}

std::optional<std::string> OpenModelFileDialog()
{
    return ShowFileDialog(FileDialogType::OpenModel);
}

std::optional<std::string> OpenTextureFileDialog()
{
    return ShowFileDialog(FileDialogType::OpenTexture);
}

std::optional<std::string> OpenSceneFileDialog()
{
    return ShowFileDialog(FileDialogType::OpenScene);
}

std::optional<std::string> SaveSceneFileDialog()
{
    return ShowFileDialog(FileDialogType::SaveScene);
}
