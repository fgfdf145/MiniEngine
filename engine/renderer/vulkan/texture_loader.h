#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TextureData
{
    int width = 0;
    int height = 0;
    int channelCount = 0;
    std::vector<std::uint8_t> pixels;

    bool IsValid() const
    {
        return width > 0 && height > 0 && !pixels.empty();
    }
};

class TextureLoader
{
public:
    static TextureData LoadRGBA8(const std::string& path, bool flipVertically = true);
};
