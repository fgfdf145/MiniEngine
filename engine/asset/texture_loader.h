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
    // Pixels are returned top-down (row 0 = top of the image), matching the
    // glTF UV convention (origin at top-left) and Vulkan texel addressing, so
    // no flip is needed anywhere in the engine. flipVertically exists only for
    // sources that store rows bottom-up.
    static TextureData LoadRGBA8(const std::string& path, bool flipVertically = false);
};
