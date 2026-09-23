#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace me
{

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

// Linear RGBA, four floats per texel, rows top-down. What .hdr and .exr files decode to, before any
// consumer picks a GPU format.
struct FloatTextureData
{
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    bool IsValid() const
    {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

class TextureLoader
{
  public:
    // .hdr and .exr, by extension and ignoring case: the files LoadRGBA32F reads.
    static bool IsFloatImageFile(const std::filesystem::path& path);
    // Scene-linear RGBA32F, rows top-down. A missing alpha is 1. Throws std::runtime_error for a file
    // that is not a float image or cannot be decoded.
    static FloatTextureData LoadRGBA32F(const std::string& path);
    // Pixels are returned top-down (row 0 = top of the image), matching the
    // glTF UV convention (origin at top-left) and Vulkan texel addressing, so
    // no flip is needed anywhere in the engine. flipVertically exists only for
    // sources that store rows bottom-up. A float image is clamped to [0, 1] and
    // its colour sRGB-encoded, for previews.
    static TextureData LoadRGBA8(const std::string& path, bool flipVertically = false);
};
}
