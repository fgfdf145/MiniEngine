#include "file_dialog.h"

#include "file_dialog_backend.h"

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

}
