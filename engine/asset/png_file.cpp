#include "png_file.h"

// The implementation is in gltf_model_loader.cpp.
#include <stb_image_write.h>

#include <stdexcept>
#include <string>

namespace me
{

void WriteRgba8Png(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels)
{
    if (width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height * 4)
    {
        throw std::runtime_error("Too few pixels for a " + std::to_string(width) + "x" + std::to_string(height) + " PNG");
    }
    if (stbi_write_png(
            path.string().c_str(),
            static_cast<int>(width),
            static_cast<int>(height),
            4,
            pixels.data(),
            static_cast<int>(width) * 4) == 0)
    {
        throw std::runtime_error("Failed to write '" + path.string() + "'");
    }
}
}
