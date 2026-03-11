#include "texture_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <stdexcept>

TextureData TextureLoader::LoadRGBA8(const std::string& path, bool flipVertically)
{
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    stbi_uc* rawPixels = stbi_load(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        throw std::runtime_error("Failed to load texture: " + path + " (" + stbi_failure_reason() + ")");
    }

    TextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.channelCount = 4;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    stbi_image_free(rawPixels);
    return texture;
}
