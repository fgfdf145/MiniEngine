#pragma once

#include "file_dialog.h"

namespace me
{

namespace platform::file_dialog
{
bool SupportsNativeFileDialogs();
std::optional<std::string> ShowFileDialog(FileDialogType type);
}
}
