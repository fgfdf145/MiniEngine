#include <engine/asset/texture_loader.h>
#include <engine/asset/texture_preparation.h>

#include <stb_image_write.h>
#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(float actual, float expected, float relativeTolerance)
{
    return std::fabs(actual - expected) <= relativeTolerance * std::max(std::fabs(expected), 1e-6f);
}

// A fresh directory per test, removed when the object goes out of scope.
class ScratchDirectory
{
  public:
    ScratchDirectory()
    {
        std::random_device random;
        m_path = std::filesystem::temp_directory_path() / ("miniengine_float_textures_" + std::to_string(random()));
        std::filesystem::create_directories(m_path);
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

// 3x2, row 0 first. Every value is exact in RGBE: each pixel's channels share its largest
// channel's exponent, and the smaller channels here are that channel over a power of two.
const std::vector<float> kHdrRgb = {
    0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 1.0f, 3.5f, 3.5f, 3.5f,
    1000.0f, 1000.0f, 1000.0f, 0.5f, 2.0f, 8.0f, 0.0f, 0.0f, 0.0f};

std::filesystem::path WriteHdr(const std::filesystem::path& directory)
{
    const std::filesystem::path path = directory / "fixture.hdr";
    Require(stbi_write_hdr(path.string().c_str(), 3, 2, 3, kHdrRgb.data()) != 0, "could not write the .hdr fixture");
    return path;
}

const std::vector<float> kExrRgba = {
    0.25f, 0.5f, 0.75f, 1.0f, 3.5f, 1000.0f, 12345.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 4.0f, 8.0f, 0.25f, 65504.0f, 0.125f, 0.0625f, 1.0f};

std::filesystem::path WriteExr(const std::filesystem::path& directory)
{
    const std::filesystem::path path = directory / "fixture.exr";
    const char* error = nullptr;
    const int result = SaveEXR(kExrRgba.data(), 3, 2, 4, 0, path.string().c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        const std::string reason = error != nullptr ? error : "unknown";
        FreeEXRErrorMessage(error);
        throw std::runtime_error("could not write the .exr fixture: " + reason);
    }
    return path;
}

template <typename Function>
bool Throws(Function&& function)
{
    try
    {
        function();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

void DetectsFloatFilesByExtension()
{
    Require(TextureLoader::IsFloatImageFile("a/b.hdr"), ".hdr is a float image");
    Require(TextureLoader::IsFloatImageFile("a/b.HDR"), ".HDR is a float image");
    Require(TextureLoader::IsFloatImageFile("b.exr"), ".exr is a float image");
    Require(TextureLoader::IsFloatImageFile("b.Exr"), ".Exr is a float image");
    Require(!TextureLoader::IsFloatImageFile("b.png"), ".png is not a float image");
    Require(!TextureLoader::IsFloatImageFile("b.jpg"), ".jpg is not a float image");
    Require(!TextureLoader::IsFloatImageFile("hdr"), "no extension is not a float image");
}

void LoadsRadianceHdr()
{
    ScratchDirectory directory;
    const FloatTextureData image = TextureLoader::LoadRGBA32F(WriteHdr(directory.Path()).string());
    Require(image.IsValid() && image.width == 3 && image.height == 2, ".hdr keeps its size");
    for (size_t texel = 0; texel < 6; ++texel)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            const float expected = kHdrRgb[texel * 3 + channel];
            const float actual = image.pixels[texel * 4 + channel];
            Require(NearlyEqual(actual, expected, 0.01f),
                    ".hdr texel " + std::to_string(texel) + " channel " + std::to_string(channel) + " is " +
                        std::to_string(actual) + ", expected " + std::to_string(expected));
        }
        Require(image.pixels[texel * 4 + 3] == 1.0f, ".hdr alpha is 1");
    }
}

void LoadsOpenExr()
{
    ScratchDirectory directory;
    const FloatTextureData image = TextureLoader::LoadRGBA32F(WriteExr(directory.Path()).string());
    Require(image.IsValid() && image.width == 3 && image.height == 2, ".exr keeps its size");
    for (size_t index = 0; index < kExrRgba.size(); ++index)
    {
        Require(NearlyEqual(image.pixels[index], kExrRgba[index], 1e-3f),
                ".exr value " + std::to_string(index) + " is " + std::to_string(image.pixels[index]) + ", expected " +
                    std::to_string(kExrRgba[index]));
    }
}

void LoadRgba8ShowsFloatFilesClampedAndEncoded()
{
    ScratchDirectory directory;
    const TextureData image = TextureLoader::LoadRGBA8(WriteHdr(directory.Path()).string());
    Require(image.width == 3 && image.height == 2 && image.pixels.size() == 3 * 2 * 4, "RGBA8 view keeps the size");
    Require(image.pixels[0] == 137, "linear 0.25 is sRGB 137, got " + std::to_string(image.pixels[0]));
    Require(image.pixels[4] == 255, "linear 1.0 is 255");
    Require(image.pixels[3 * 4] == 255, "1000 clamps to 255");
    Require(image.pixels[5 * 4] == 0, "0 stays 0");
    Require(image.pixels[3] == 255, "alpha is 255");
}

void RejectsWhatIsNotAFloatImage()
{
    ScratchDirectory directory;

    const std::filesystem::path png = directory.Path() / "plain.png";
    const std::uint8_t pixel[4] = {10, 20, 30, 255};
    Require(stbi_write_png(png.string().c_str(), 1, 1, 4, pixel, 4) != 0, "could not write the .png fixture");
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(png.string()); }), "a .png is not a float image");

    // A PNG under an .hdr name: stbi_loadf would convert it from LDR, which is not what .hdr means.
    const std::filesystem::path disguised = directory.Path() / "disguised.hdr";
    std::filesystem::copy_file(png, disguised);
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(disguised.string()); }), "a PNG named .hdr is refused");

    const std::filesystem::path exr = WriteExr(directory.Path());
    std::filesystem::resize_file(exr, 20);
    Require(Throws([&]() { TextureLoader::LoadRGBA32F(exr.string()); }), "a truncated .exr throws");

    Require(Throws([&]() { TextureLoader::LoadRGBA32F((directory.Path() / "missing.exr").string()); }),
            "a missing file throws");
}

void PacksHalfFloats()
{
    const auto hex = [](std::uint16_t value)
    {
        char text[8] = {};
        std::snprintf(text, sizeof(text), "0x%04X", value);
        return std::string(text);
    };
    const auto expect = [&](float input, std::uint16_t expected, const std::string& label)
    {
        const std::uint16_t actual = PackHalfFloat(input);
        Require(actual == expected, label + " packs to " + hex(actual) + ", expected " + hex(expected));
    };
    expect(0.0f, 0x0000, "0");
    expect(1.0f, 0x3C00, "1");
    expect(-2.0f, 0xC000, "-2");
    expect(65504.0f, 0x7BFF, "65504");
    expect(1.0e6f, 0x7BFF, "1e6");
    expect(std::numeric_limits<float>::infinity(), 0x7BFF, "+inf");
    expect(-std::numeric_limits<float>::infinity(), 0xFBFF, "-inf");
    expect(std::numeric_limits<float>::quiet_NaN(), 0x0000, "NaN");
    expect(std::ldexp(1.0f, -15), 0x0200, "the subnormal 2^-15");
}

void PacksWholeImages()
{
    FloatTextureData image{};
    image.width = 2;
    image.height = 1;
    image.pixels = {1.0f, -2.0f, 1.0e6f, 1.0f, 0.0f, 0.5f, 0.25f, 0.0f};
    const HalfFloatTextureData packed = PackRgba16Float(image);
    Require(packed.IsValid() && packed.width == 2 && packed.height == 1, "packing keeps the size");
    const std::vector<std::uint16_t> expected = {0x3C00, 0xC000, 0x7BFF, 0x3C00, 0x0000, 0x3800, 0x3400, 0x0000};
    Require(packed.texels == expected, "packing converts every channel in order");
}

void PreparesFloatFilesUncompressed()
{
    ScratchDirectory images;
    ScratchDirectory cache;
    const std::filesystem::path hdr = WriteHdr(images.Path());
    const PreparedTexture prepared = PrepareTexture(hdr.string(), TextureUsage::Color, true, cache.Path());
    Require(prepared.halfFloat.has_value() && prepared.halfFloat->IsValid(), "a float file prepares as half floats");
    Require(!prepared.compressed.has_value(), "a float file is never block-compressed");
    Require(prepared.rgba.pixels.empty(), "a float file has no RGBA8 form");
    Require(prepared.halfFloat->texels[3 * 4] == 0x63D0, "1000 survives as half 1000 (0x63D0)");
    Require(std::filesystem::is_empty(cache.Path()), "a float file writes nothing to the texture cache");
}
}

int main()
{
    try
    {
        DetectsFloatFilesByExtension();
        LoadsRadianceHdr();
        LoadsOpenExr();
        LoadRgba8ShowsFloatFilesClampedAndEncoded();
        RejectsWhatIsNotAFloatImage();
        PacksHalfFloats();
        PacksWholeImages();
        PreparesFloatFilesUncompressed();
    }
    catch (const std::exception& error)
    {
        std::cerr << "float texture tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "float texture tests passed\n";
    return 0;
}
