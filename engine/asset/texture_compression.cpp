#include "texture_compression.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace me
{

namespace
{
void InitializeEncoders()
{
    // Both encoders build lookup tables once; after that, blocks encode independently and
    // concurrently.
    static std::once_flag once;
    std::call_once(once, []()
                   {
                       bc7enc_compress_block_init();
                       rgbcx::init();
                   });
}

// A copy of the image as four-channel RGBA8, whatever it was decoded as.
TextureData ToRgba8(const TextureData& image)
{
    if (!image.IsValid())
    {
        throw std::runtime_error("Cannot compress an invalid texture");
    }
    if (image.channelCount == 4 || image.channelCount == 0)
    {
        TextureData copy = image;
        copy.channelCount = 4;
        return copy;
    }
    throw std::runtime_error("Texture compression expects RGBA8 pixels");
}

// Copies the 4x4 block at (blockX, blockY), repeating the last row and column past the image
// edge so partial blocks encode colours that are actually in the image.
void GatherBlock(const TextureData& level, uint32_t blockX, uint32_t blockY, uint8_t (&block)[16 * 4])
{
    const uint32_t width = static_cast<uint32_t>(level.width);
    const uint32_t height = static_cast<uint32_t>(level.height);
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t x = std::min(blockX * 4 + i % 4, width - 1);
        const uint32_t y = std::min(blockY * 4 + i / 4, height - 1);
        const uint8_t* source = &level.pixels[(static_cast<size_t>(y) * width + x) * 4];
        std::copy(source, source + 4, &block[i * 4]);
    }
}

CompressedTextureLevel EncodeLevel(const TextureData& level, TextureUsage usage, const bc7enc_compress_block_params& params)
{
    CompressedTextureLevel encoded{};
    encoded.width = static_cast<uint32_t>(level.width);
    encoded.height = static_cast<uint32_t>(level.height);
    const uint32_t blocksX = BlockCount(encoded.width);
    const uint32_t blocksY = BlockCount(encoded.height);
    encoded.blocks.resize(static_cast<size_t>(blocksX) * blocksY * kCompressedBlockBytes);

    const auto encodeRows = [&](uint32_t firstRow, uint32_t endRow)
    {
        uint8_t block[16 * 4];
        for (uint32_t by = firstRow; by < endRow; ++by)
        {
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                GatherBlock(level, bx, by, block);
                uint8_t* destination = &encoded.blocks[(static_cast<size_t>(by) * blocksX + bx) * kCompressedBlockBytes];
                if (usage == TextureUsage::Normal)
                {
                    // The high-quality BC5 search was measured at about 230 times the encode time
                    // on Sponza normal maps for no better maximum error, so the standard encoder
                    // is used.
                    rgbcx::encode_bc5(destination, block, 0, 1, 4);
                }
                else
                {
                    bc7enc_compress_block(destination, block, &params);
                }
            }
        }
    };

    // Blocks are independent, so large levels are split into bands of rows encoded concurrently.
    // Callers may compress several textures in parallel as well; a model with one or two large
    // textures would otherwise encode each on a single thread. Small levels are not worth a thread.
    constexpr uint32_t kMinRowsPerBand = 16;
    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t bandCount = std::min(hardwareThreads, blocksY / kMinRowsPerBand);
    if (bandCount <= 1)
    {
        encodeRows(0, blocksY);
        return encoded;
    }

    std::vector<std::future<void>> bands;
    bands.reserve(bandCount);
    for (uint32_t band = 0; band < bandCount; ++band)
    {
        const uint32_t firstRow = blocksY * band / bandCount;
        const uint32_t endRow = blocksY * (band + 1) / bandCount;
        bands.push_back(std::async(std::launch::async, encodeRows, firstRow, endRow));
    }
    for (std::future<void>& band : bands)
    {
        band.get();
    }
    return encoded;
}
}

CompressedTextureFormat FormatForUsage(TextureUsage usage)
{
    switch (usage)
    {
    case TextureUsage::Color:
        return CompressedTextureFormat::Bc7Srgb;
    case TextureUsage::Normal:
        return CompressedTextureFormat::Bc5Unorm;
    case TextureUsage::Data:
        return CompressedTextureFormat::Bc7Unorm;
    }
    throw std::runtime_error("Unknown texture usage");
}

std::vector<TextureData> BuildMipChain(const TextureData& image, TextureUsage usage)
{
    std::vector<TextureData> levels;
    levels.push_back(ToRgba8(image));

    while (levels.back().width > 1 || levels.back().height > 1)
    {
        const TextureData& previous = levels.back();
        TextureData next{};
        next.width = std::max(previous.width / 2, 1);
        next.height = std::max(previous.height / 2, 1);
        next.channelCount = 4;
        next.pixels.resize(static_cast<size_t>(next.width) * next.height * 4);

        // Colour is sRGB-encoded with real alpha, so it is filtered in linear light with alpha
        // weighting. Normals and data are filtered as stored, and their fourth channel may be
        // data rather than coverage, so it must not weight the others.
        const unsigned char* result =
            usage == TextureUsage::Color
                ? stbir_resize_uint8_srgb(
                      previous.pixels.data(), previous.width, previous.height, 0,
                      next.pixels.data(), next.width, next.height, 0,
                      STBIR_RGBA)
                : stbir_resize_uint8_linear(
                      previous.pixels.data(), previous.width, previous.height, 0,
                      next.pixels.data(), next.width, next.height, 0,
                      STBIR_4CHANNEL);
        if (result == nullptr)
        {
            throw std::runtime_error("Failed to build a texture mip level");
        }
        levels.push_back(std::move(next));
    }
    return levels;
}

CompressedTexture CompressTexture(const TextureData& image, TextureUsage usage)
{
    InitializeEncoders();

    // The fastest settings: uber level 0 and no two-subset partitions (mode 1). Measured on a Sponza
    // base colour texture, dropping the partitions encodes 5.5 times faster for 0.23 dB of PSNR
    // (45.9 to 45.7 dB), which is what makes compressing a whole scene on first load tolerable.
    bc7enc_compress_block_params params{};
    bc7enc_compress_block_params_init(&params);
    if (usage != TextureUsage::Color)
    {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    params.m_uber_level = 0;
    params.m_max_partitions = 0;

    CompressedTexture compressed{};
    compressed.format = FormatForUsage(usage);
    for (const TextureData& level : BuildMipChain(image, usage))
    {
        compressed.levels.push_back(EncodeLevel(level, usage, params));
    }
    return compressed;
}
}
