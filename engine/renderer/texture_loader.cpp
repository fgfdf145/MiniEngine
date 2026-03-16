#include "texture_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string ReadNextPortableMapToken(std::istream& input)
{
    std::string token;

    while (input >> token)
    {
        if (!token.empty() && token[0] == '#')
        {
            std::string comment;
            std::getline(input, comment);
            continue;
        }

        return token;
    }

    throw std::runtime_error("Unexpected end of portable pixmap file");
}

TextureData LoadPortablePixmap(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        throw std::runtime_error("Failed to open portable pixmap: " + path);
    }

    const std::string magic = ReadNextPortableMapToken(input);
    if (magic != "P3")
    {
        throw std::runtime_error("Unsupported portable pixmap format: " + magic);
    }

    const int width = std::stoi(ReadNextPortableMapToken(input));
    const int height = std::stoi(ReadNextPortableMapToken(input));
    const int maxValue = std::stoi(ReadNextPortableMapToken(input));
    if (width <= 0 || height <= 0 || maxValue <= 0)
    {
        throw std::runtime_error("Invalid portable pixmap header");
    }

    TextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.channelCount = 4;
    texture.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const float scale = 255.0f / static_cast<float>(maxValue);
    for (int i = 0; i < width * height; ++i)
    {
        const int red = std::stoi(ReadNextPortableMapToken(input));
        const int green = std::stoi(ReadNextPortableMapToken(input));
        const int blue = std::stoi(ReadNextPortableMapToken(input));

        texture.pixels[static_cast<size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(red * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(green * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(blue * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 3] = 255;
    }

    return texture;
}
}

TextureData TextureLoader::LoadRGBA8(const std::string& path, bool flipVertically)
{
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    stbi_uc* rawPixels = stbi_load(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        TextureData texture = LoadPortablePixmap(path);
        if (flipVertically)
        {
            const size_t rowSize = static_cast<size_t>(texture.width) * 4;
            std::vector<std::uint8_t> flipped(texture.pixels.size());
            for (int y = 0; y < texture.height; ++y)
            {
                const size_t sourceOffset = static_cast<size_t>(y) * rowSize;
                const size_t destinationOffset = static_cast<size_t>(texture.height - 1 - y) * rowSize;
                std::memcpy(flipped.data() + destinationOffset, texture.pixels.data() + sourceOffset, rowSize);
            }
            texture.pixels = std::move(flipped);
        }
        return texture;
    }

    TextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.channelCount = 4;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    stbi_image_free(rawPixels);
    return texture;
}
