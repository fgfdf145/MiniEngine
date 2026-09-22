#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace me
{

namespace AtomicFile
{
// Writes the content to "<path>.tmp" and renames it over the path, so a crash
// or a full disk leaves either the old file or the new one, never a truncated
// file. Returns false and fills `error` (when given) on failure; the old file
// is untouched and the temporary is removed.
bool Write(const std::filesystem::path& path, std::string_view content, std::string* error = nullptr);
}
}
