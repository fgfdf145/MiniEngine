#include "texture_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

namespace
{
std::string LowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return ext;
}

bool IsPortableMapExtension(const std::string& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".ppm" || ext == ".pgm" || ext == ".pbm";
}

void FlipRows(TextureData& texture)
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

FloatTextureData LoadRadianceHdr(const std::string& path)
{
    // stbi_loadf also accepts LDR files and converts them with a gamma curve, which would make a PNG
    // renamed to .hdr load as something it is not.
    if (!stbi_is_hdr(path.c_str()))
    {
        throw std::runtime_error("'" + path + "' is not a Radiance HDR image");
    }
    int width = 0;
    int height = 0;
    int channelCount = 0;
    float* rawPixels = stbi_loadf(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Failed to load texture '" + path + "': " + (reason ? reason : "unknown error"));
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(rawPixels);
    return texture;
}

FloatTextureData LoadOpenExr(const std::string& path)
{
    float* rawPixels = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    const int result = LoadEXR(&rawPixels, &width, &height, path.c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        const std::string reason = error != nullptr ? error : "error code " + std::to_string(result);
        FreeEXRErrorMessage(error);
        throw std::runtime_error("Failed to load texture '" + path + "': " + reason);
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::free(rawPixels);
    return texture;
}

// NaN fails the comparison and ends up 0.
float Saturate(float value)
{
    return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
}

float LinearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// What a float image looks like through the RGBA8 path: clamped to [0, 1], colour sRGB-encoded.
TextureData ToDisplayRgba8(const FloatTextureData& image)
{
    TextureData texture{};
    texture.width = image.width;
    texture.height = image.height;
    texture.channelCount = 4;
    texture.pixels.resize(image.pixels.size());
    for (size_t index = 0; index < image.pixels.size(); ++index)
    {
        const float value = Saturate(image.pixels[index]);
        const bool isAlpha = index % 4 == 3;
        texture.pixels[index] = static_cast<std::uint8_t>(std::lround((isAlpha ? value : LinearToSrgb(value)) * 255.0f));
    }
    return texture;
}

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

bool TextureLoader::IsFloatImageFile(const std::filesystem::path& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".hdr" || ext == ".exr";
}

FloatTextureData TextureLoader::LoadRGBA32F(const std::string& path)
{
    if (!IsFloatImageFile(path))
    {
        throw std::runtime_error("'" + path + "' is not a floating-point image (.hdr or .exr)");
    }
    return LowerExtension(path) == ".exr" ? LoadOpenExr(path) : LoadRadianceHdr(path);
}

TextureData TextureLoader::LoadRGBA8(const std::string& path, bool flipVertically)
{
    if (IsFloatImageFile(path))
    {
        TextureData texture = ToDisplayRgba8(LoadRGBA32F(path));
        if (flipVertically)
        {
            FlipRows(texture);
        }
        return texture;
    }

    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    stbi_uc* rawPixels = stbi_load(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        if (!IsPortableMapExtension(path))
        {
            const char* reason = stbi_failure_reason();
            throw std::runtime_error(
                "Failed to load texture '" + path + "': " +
                (reason ? reason : "unknown error"));
        }

        TextureData texture = LoadPortablePixmap(path);
        if (flipVertically)
        {
            FlipRows(texture);
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
}
