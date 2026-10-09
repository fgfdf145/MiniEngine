#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace me
{

// Writes RGBA8 pixels (rows from the top, width * height * 4 bytes) as an RGBA PNG. Throws
// std::runtime_error when the pixels are too few or the file cannot be written.
void WriteRgba8Png(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels);
}
