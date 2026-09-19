#include <engine/asset/compressed_texture_cache.h>
#include <engine/asset/texture_compression.h>

#include <bc7decomp.h>
#include <rgbcx.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

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

TextureData MakeImage(int width, int height)
{
    TextureData image{};
    image.width = width;
    image.height = height;
    image.channelCount = 4;
    image.pixels.assign(static_cast<size_t>(width) * height * 4, 255);
    return image;
}

uint8_t* Pixel(TextureData& image, int x, int y)
{
    return &image.pixels[(static_cast<size_t>(y) * image.width + x) * 4];
}

// A smooth image with detail in every channel: the kind of content BC7 is expected to hold well.
TextureData MakeGradient(int size)
{
    TextureData image = MakeImage(size, size);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            uint8_t* pixel = Pixel(image, x, y);
            pixel[0] = static_cast<uint8_t>(x * 255 / (size - 1));
            pixel[1] = static_cast<uint8_t>(y * 255 / (size - 1));
            pixel[2] = static_cast<uint8_t>(128 + 100 * std::sin(0.1 * (x + y)));
            pixel[3] = 255;
        }
    }
    return image;
}

// Decodes level 0 of a BC7 texture back to RGBA8.
TextureData DecodeBc7(const CompressedTextureLevel& level)
{
    TextureData image = MakeImage(static_cast<int>(level.width), static_cast<int>(level.height));
    const uint32_t blocksX = BlockCount(level.width);
    const uint32_t blocksY = BlockCount(level.height);
    for (uint32_t by = 0; by < blocksY; ++by)
    {
        for (uint32_t bx = 0; bx < blocksX; ++bx)
        {
            bc7decomp::color_rgba decoded[16];
            bc7decomp::unpack_bc7(&level.blocks[(by * blocksX + bx) * kCompressedBlockBytes], decoded);
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint32_t x = bx * 4 + i % 4;
                const uint32_t y = by * 4 + i / 4;
                if (x < level.width && y < level.height)
                {
                    std::copy(decoded[i].m_comps, decoded[i].m_comps + 4, Pixel(image, static_cast<int>(x), static_cast<int>(y)));
                }
            }
        }
    }
    return image;
}

double RgbPsnr(const TextureData& a, const TextureData& b)
{
    double squaredError = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
    {
        if (i % 4 == 3)
        {
            continue;
        }
        const double difference = static_cast<double>(a.pixels[i]) - static_cast<double>(b.pixels[i]);
        squaredError += difference * difference;
        ++count;
    }
    const double mse = squaredError / static_cast<double>(count);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void MipChainHalvesToOnePixel()
{
    const std::vector<TextureData> large = BuildMipChain(MakeImage(4096, 4096), TextureUsage::Color);
    Require(large.size() == 13, "a 4096 square needs 13 levels, got " + std::to_string(large.size()));
    Require(large.back().width == 1 && large.back().height == 1, "the chain must end at 1x1");

    const std::vector<TextureData> odd = BuildMipChain(MakeImage(5, 3), TextureUsage::Data);
    Require(odd.size() == 3, "5x3 needs three levels");
    Require(odd[1].width == 2 && odd[1].height == 1, "5x3 halves to 2x1");
    Require(odd[2].width == 1 && odd[2].height == 1, "2x1 halves to 1x1");

    Require(BuildMipChain(MakeImage(1, 1), TextureUsage::Normal).size() == 1, "1x1 is its own chain");
}

void SrgbDownsampleAveragesInLinearLight()
{
    TextureData checker = MakeImage(2, 2);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
            std::fill(Pixel(checker, x, y), Pixel(checker, x, y) + 3, value);
        }
    }

    // Half the light of white is linear 0.5, which sRGB encodes as 188; averaging the encoded
    // values instead would give 128 and darken every colour texture's distant mips.
    const int color = BuildMipChain(checker, TextureUsage::Color)[1].pixels[0];
    Require(std::abs(color - 188) <= 2, "colour must be averaged in linear light, got " + std::to_string(color));
    const int data = BuildMipChain(checker, TextureUsage::Data)[1].pixels[0];
    Require(std::abs(data - 128) <= 2, "data must be averaged as stored, got " + std::to_string(data));
}

void Bc7RoundTripIsAccurate()
{
    const TextureData source = MakeGradient(64);
    const CompressedTexture compressed = CompressTexture(source, TextureUsage::Color);
    Require(compressed.format == CompressedTextureFormat::Bc7Srgb, "colour textures are BC7 sRGB");
    Require(compressed.levels.size() == 7, "64 to 1 is seven levels");

    // Red along x and green along y put every block's colours on a plane, which the single-subset
    // modes the encoder is configured for can only approximate with a line: this image is their
    // worst case, and scores about 37 dB where Sponza's textures score about 45.7 dB.
    const double psnr = RgbPsnr(source, DecodeBc7(compressed.levels[0]));
    Require(psnr > 35.0, "BC7 PSNR too low: " + std::to_string(psnr));
}

void Bc7KeepsAlpha()
{
    TextureData source = MakeImage(16, 16);
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            Pixel(source, x, y)[3] = x < 8 ? 0 : 255;
        }
    }
    const TextureData decoded = DecodeBc7(CompressTexture(source, TextureUsage::Color).levels[0]);
    for (size_t i = 3; i < source.pixels.size(); i += 4)
    {
        Require(std::abs(static_cast<int>(source.pixels[i]) - decoded.pixels[i]) <= 8, "BC7 must keep a cutout's alpha");
    }
}

void Bc5RoundTripIsAccurate()
{
    const TextureData source = MakeGradient(64);
    const CompressedTexture compressed = CompressTexture(source, TextureUsage::Normal);
    Require(compressed.format == CompressedTextureFormat::Bc5Unorm, "normal maps are BC5");

    const CompressedTextureLevel& level = compressed.levels[0];
    int worst = 0;
    for (uint32_t by = 0; by < BlockCount(level.height); ++by)
    {
        for (uint32_t bx = 0; bx < BlockCount(level.width); ++bx)
        {
            uint8_t decoded[16 * 4] = {};
            rgbcx::unpack_bc5(&level.blocks[(by * BlockCount(level.width) + bx) * kCompressedBlockBytes], decoded, 0, 1, 4);
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint8_t* expected = &source.pixels[((by * 4 + i / 4) * level.width + bx * 4 + i % 4) * 4];
                worst = std::max(worst, std::abs(static_cast<int>(expected[0]) - decoded[i * 4 + 0]));
                worst = std::max(worst, std::abs(static_cast<int>(expected[1]) - decoded[i * 4 + 1]));
            }
        }
    }
    Require(worst <= 2, "BC5 error too large: " + std::to_string(worst));
}

void OddSizesEncodeWholeBlocks()
{
    const CompressedTexture compressed = CompressTexture(MakeImage(5, 3), TextureUsage::Data);
    Require(compressed.format == CompressedTextureFormat::Bc7Unorm, "data textures are BC7 unorm");
    Require(compressed.levels.size() == 3, "5x3 compresses three levels");
    Require(compressed.levels[0].width == 5 && compressed.levels[0].height == 3, "levels keep their pixel size");
    Require(compressed.levels[0].blocks.size() == 2 * 1 * kCompressedBlockBytes, "5x3 is 2x1 blocks");
    Require(compressed.levels[2].blocks.size() == kCompressedBlockBytes, "1x1 is one block");
}

// A fresh directory per test run, removed when the object goes out of scope.
class ScratchDirectory
{
  public:
    ScratchDirectory()
    {
        std::random_device random;
        m_path = std::filesystem::temp_directory_path() / ("miniengine_texture_cache_" + std::to_string(random()));
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

std::filesystem::path WritePng(const std::filesystem::path& directory, const std::string& name)
{
    const TextureData image = MakeGradient(20);
    const std::filesystem::path path = directory / name;
    Require(
        stbi_write_png(path.string().c_str(), image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0,
        "could not write the test image");
    return path;
}

size_t CountCacheFiles(const std::filesystem::path& cacheDirectory)
{
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory))
    {
        count += entry.path().extension() == ".metex" ? 1 : 0;
    }
    return count;
}

bool SameTexture(const CompressedTexture& a, const CompressedTexture& b)
{
    if (a.format != b.format || a.levels.size() != b.levels.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.levels.size(); ++i)
    {
        if (a.levels[i].width != b.levels[i].width || a.levels[i].height != b.levels[i].height ||
            a.levels[i].blocks != b.levels[i].blocks)
        {
            return false;
        }
    }
    return true;
}

void CacheMissThenHit()
{
    ScratchDirectory scratch;
    const std::filesystem::path image = WritePng(scratch.Path(), "gradient.png");
    const std::filesystem::path cache = scratch.Path() / "cache";

    const CompressedTextureLoad first = LoadOrCompressTexture(image, TextureUsage::Color, cache);
    Require(!first.cacheHit, "the first load must compress");
    Require(CountCacheFiles(cache) == 1, "the first load must write one cache file");

    const CompressedTextureLoad second = LoadOrCompressTexture(image, TextureUsage::Color, cache);
    Require(second.cacheHit, "the second load must come from the cache");
    Require(SameTexture(first.texture, second.texture), "the cached texture must equal the compressed one");
}

void ChangedFileMisses()
{
    ScratchDirectory scratch;
    const std::filesystem::path image = WritePng(scratch.Path(), "gradient.png");
    const std::filesystem::path cache = scratch.Path() / "cache";
    LoadOrCompressTexture(image, TextureUsage::Color, cache);

    std::filesystem::last_write_time(image, std::filesystem::last_write_time(image) + std::chrono::hours(1));
    Require(!LoadOrCompressTexture(image, TextureUsage::Color, cache).cacheHit, "an edited file must be compressed again");
}

void UsageIsPartOfTheKey()
{
    ScratchDirectory scratch;
    const std::filesystem::path image = WritePng(scratch.Path(), "gradient.png");
    const std::filesystem::path cache = scratch.Path() / "cache";
    LoadOrCompressTexture(image, TextureUsage::Color, cache);

    Require(!LoadOrCompressTexture(image, TextureUsage::Data, cache).cacheHit, "another usage is another texture");
    Require(CountCacheFiles(cache) == 2, "each usage has its own cache file");
    Require(
        BuildCompressedTextureKey(image, TextureUsage::Color) != BuildCompressedTextureKey(image, TextureUsage::Data),
        "usage must be part of the key");
}

void DamagedFilesMiss()
{
    ScratchDirectory scratch;
    const std::filesystem::path image = WritePng(scratch.Path(), "gradient.png");
    const std::filesystem::path cache = scratch.Path() / "cache";
    const CompressedTexture texture = LoadOrCompressTexture(image, TextureUsage::Color, cache).texture;
    const std::string key = BuildCompressedTextureKey(image, TextureUsage::Color);
    const std::filesystem::path file = CompressedTextureCacheFile(cache, key);
    Require(ReadCompressedTexture(file, key).has_value(), "an intact file must read back");

    Require(!ReadCompressedTexture(file, key + "x").has_value(), "a file written under another key must be a miss");

    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 7);
    Require(!ReadCompressedTexture(file, key).has_value(), "a truncated file must be a miss");

    Require(WriteCompressedTexture(file, key, texture), "rewriting the file must succeed");
    Require(ReadCompressedTexture(file, key).has_value(), "a rewritten file must read back");
    Require(!ReadCompressedTexture(scratch.Path() / "missing.metex", key).has_value(), "a missing file must be a miss");
}
}

int main()
{
    try
    {
        MipChainHalvesToOnePixel();
        SrgbDownsampleAveragesInLinearLight();
        Bc7RoundTripIsAccurate();
        Bc7KeepsAlpha();
        Bc5RoundTripIsAccurate();
        OddSizesEncodeWholeBlocks();
        CacheMissThenHit();
        ChangedFileMisses();
        UsageIsPartOfTheKey();
        DamagedFilesMiss();
    }
    catch (const std::exception& error)
    {
        std::cerr << "texture compression tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "texture compression tests passed\n";
    return 0;
}
