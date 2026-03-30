#pragma once

#include "file_dialog.h"

namespace platform::file_dialog
{
bool SupportsNativeFileDialogs();
std::optional<std::string> ShowFileDialog(FileDialogType type);
}
