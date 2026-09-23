#include <engine/asset/texture_loader.h>

#include <stb_image_write.h>
#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
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
    }
    catch (const std::exception& error)
    {
        std::cerr << "float texture tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "float texture tests passed\n";
    return 0;
}
