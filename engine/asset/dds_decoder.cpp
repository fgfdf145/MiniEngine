#include "dds_decoder.h"

#include <bc7decomp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace me
{

namespace
{
constexpr size_t kMagicSize = 4;
constexpr size_t kHeaderSize = 124;
constexpr size_t kDx10HeaderSize = 20;

// DDS_PIXELFORMAT.dwFlags.
constexpr std::uint32_t kPixelAlphaPixels = 0x1;
constexpr std::uint32_t kPixelAlpha = 0x2;
constexpr std::uint32_t kPixelFourCc = 0x4;
constexpr std::uint32_t kPixelRgb = 0x40;
constexpr std::uint32_t kPixelLuminance = 0x20000;

// The DXGI formats a DX10 header can name that decode here.
constexpr std::uint32_t kDxgiR8G8B8A8Unorm = 28;
constexpr std::uint32_t kDxgiR8G8B8A8Srgb = 29;
constexpr std::uint32_t kDxgiBc1Unorm = 71;
constexpr std::uint32_t kDxgiBc1Srgb = 72;
constexpr std::uint32_t kDxgiBc2Unorm = 74;
constexpr std::uint32_t kDxgiBc2Srgb = 75;
constexpr std::uint32_t kDxgiBc3Unorm = 77;
constexpr std::uint32_t kDxgiBc3Srgb = 78;
constexpr std::uint32_t kDxgiBc4Unorm = 80;
constexpr std::uint32_t kDxgiBc5Unorm = 83;
constexpr std::uint32_t kDxgiB8G8R8A8Unorm = 87;
constexpr std::uint32_t kDxgiB8G8R8X8Unorm = 88;
constexpr std::uint32_t kDxgiB8G8R8A8Srgb = 91;
constexpr std::uint32_t kDxgiB8G8R8X8Srgb = 93;
constexpr std::uint32_t kDxgiBc7Unorm = 98;
constexpr std::uint32_t kDxgiBc7Srgb = 99;

enum class BlockFormat
{
    Bc1,
    Bc2,
    Bc3,
    Bc4,
    Bc5,
    Bc7,
};

struct ChannelMasks
{
    std::uint32_t bitCount = 0;
    std::array<std::uint32_t, 4> masks{}; // r, g, b, a
    bool luminance = false;
};

constexpr std::uint32_t FourCc(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

std::uint32_t ReadU32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t ReadU16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::array<std::uint8_t, 4> Expand565(std::uint16_t color)
{
    const int r = (color >> 11) & 31;
    const int g = (color >> 5) & 63;
    const int b = color & 31;
    return {
        static_cast<std::uint8_t>((r << 3) | (r >> 2)),
        static_cast<std::uint8_t>((g << 2) | (g >> 4)),
        static_cast<std::uint8_t>((b << 3) | (b >> 2)),
        255};
}

// The colour half of BC1/BC2/BC3. BC2 and BC3 always use the four-colour mode, whatever the
// endpoint order; only a BC1 block with c0 <= c1 has three colours and a transparent black.
void DecodeColorBlock(const std::uint8_t* block, bool allowThreeColor, std::uint8_t out[16][4])
{
    const std::uint16_t c0 = ReadU16(block);
    const std::uint16_t c1 = ReadU16(block + 2);
    std::array<std::array<std::uint8_t, 4>, 4> palette{};
    palette[0] = Expand565(c0);
    palette[1] = Expand565(c1);
    if (c0 > c1 || !allowThreeColor)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            palette[2][channel] = static_cast<std::uint8_t>((2 * palette[0][channel] + palette[1][channel] + 1) / 3);
            palette[3][channel] = static_cast<std::uint8_t>((palette[0][channel] + 2 * palette[1][channel] + 1) / 3);
        }
        palette[2][3] = 255;
        palette[3][3] = 255;
    }
    else
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            palette[2][channel] = static_cast<std::uint8_t>((palette[0][channel] + palette[1][channel]) / 2);
        }
        palette[2][3] = 255;
        palette[3] = {0, 0, 0, 0};
    }

    const std::uint32_t indices = ReadU32(block + 4);
    for (int texel = 0; texel < 16; ++texel)
    {
        const std::array<std::uint8_t, 4>& color = palette[(indices >> (2 * texel)) & 3];
        std::memcpy(out[texel], color.data(), 4);
    }
}

// A BC4 block (also BC3's alpha and each BC5 channel): two endpoints and sixteen 3-bit indices.
void DecodeSingleChannelBlock(const std::uint8_t* block, std::uint8_t out[16])
{
    const int a0 = block[0];
    const int a1 = block[1];
    std::array<int, 8> palette{a0, a1, 0, 0, 0, 0, 0, 0};
    if (a0 > a1)
    {
        for (int step = 1; step < 7; ++step)
        {
            palette[step + 1] = ((7 - step) * a0 + step * a1 + 3) / 7;
        }
    }
    else
    {
        for (int step = 1; step < 5; ++step)
        {
            palette[step + 1] = ((5 - step) * a0 + step * a1 + 2) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    std::uint64_t indices = 0;
    for (int byte = 0; byte < 6; ++byte)
    {
        indices |= static_cast<std::uint64_t>(block[2 + byte]) << (8 * byte);
    }
    for (int texel = 0; texel < 16; ++texel)
    {
        out[texel] = static_cast<std::uint8_t>(palette[(indices >> (3 * texel)) & 7]);
    }
}

size_t BlockBytes(BlockFormat format)
{
    return format == BlockFormat::Bc1 || format == BlockFormat::Bc4 ? 8 : 16;
}

void DecodeBlock(BlockFormat format, const std::uint8_t* block, std::uint8_t out[16][4])
{
    switch (format)
    {
    case BlockFormat::Bc1:
        DecodeColorBlock(block, true, out);
        break;
    case BlockFormat::Bc2:
        DecodeColorBlock(block + 8, false, out);
        for (int texel = 0; texel < 16; ++texel)
        {
            const int nibble = (block[texel / 2] >> (4 * (texel & 1))) & 15;
            out[texel][3] = static_cast<std::uint8_t>(nibble * 17);
        }
        break;
    case BlockFormat::Bc3:
    {
        DecodeColorBlock(block + 8, false, out);
        std::uint8_t alpha[16];
        DecodeSingleChannelBlock(block, alpha);
        for (int texel = 0; texel < 16; ++texel)
        {
            out[texel][3] = alpha[texel];
        }
        break;
    }
    case BlockFormat::Bc4:
    {
        std::uint8_t red[16];
        DecodeSingleChannelBlock(block, red);
        for (int texel = 0; texel < 16; ++texel)
        {
            out[texel][0] = red[texel];
            out[texel][1] = red[texel];
            out[texel][2] = red[texel];
            out[texel][3] = 255;
        }
        break;
    }
    case BlockFormat::Bc5:
    {
        std::uint8_t red[16];
        std::uint8_t green[16];
        DecodeSingleChannelBlock(block, red);
        DecodeSingleChannelBlock(block + 8, green);
        for (int texel = 0; texel < 16; ++texel)
        {
            out[texel][0] = red[texel];
            out[texel][1] = green[texel];
            out[texel][2] = 0;
            out[texel][3] = 255;
        }
        break;
    }
    case BlockFormat::Bc7:
    {
        bc7decomp::color_rgba texels[16];
        if (!bc7decomp::unpack_bc7(block, texels))
        {
            // A reserved mode: the format decodes it to transparent black.
            std::memset(out, 0, 16 * 4);
            break;
        }
        for (int texel = 0; texel < 16; ++texel)
        {
            std::memcpy(out[texel], texels[texel].m_comps, 4);
        }
        break;
    }
    }
}

TextureData DecodeBlocks(
    BlockFormat format,
    const std::uint8_t* data,
    size_t available,
    int width,
    int height,
    const std::string& source)
{
    const size_t blocksX = (static_cast<size_t>(width) + 3) / 4;
    const size_t blocksY = (static_cast<size_t>(height) + 3) / 4;
    const size_t needed = blocksX * blocksY * BlockBytes(format);
    if (available < needed)
    {
        throw std::runtime_error("DDS texture '" + source + "' is truncated");
    }

    TextureData image;
    image.width = width;
    image.height = height;
    image.channelCount = 4;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    std::uint8_t texels[16][4];
    for (size_t by = 0; by < blocksY; ++by)
    {
        for (size_t bx = 0; bx < blocksX; ++bx)
        {
            DecodeBlock(format, data + (by * blocksX + bx) * BlockBytes(format), texels);
            for (size_t y = 0; y < 4; ++y)
            {
                const size_t py = by * 4 + y;
                if (py >= static_cast<size_t>(height))
                {
                    break;
                }
                for (size_t x = 0; x < 4; ++x)
                {
                    const size_t px = bx * 4 + x;
                    if (px >= static_cast<size_t>(width))
                    {
                        break;
                    }
                    std::memcpy(&image.pixels[(py * static_cast<size_t>(width) + px) * 4], texels[y * 4 + x], 4);
                }
            }
        }
    }
    return image;
}

std::uint8_t ExtractChannel(std::uint32_t pixel, std::uint32_t mask)
{
    if (mask == 0)
    {
        return 0;
    }
    int shift = 0;
    while (((mask >> shift) & 1u) == 0)
    {
        ++shift;
    }
    const std::uint32_t shifted = mask >> shift;
    int bits = 0;
    while (bits < 32 && ((shifted >> bits) & 1u) != 0)
    {
        ++bits;
    }
    const std::uint64_t maximum = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
    const std::uint64_t value = (pixel & mask) >> shift;
    return static_cast<std::uint8_t>((value * 255 + maximum / 2) / maximum);
}

TextureData DecodeMasked(
    const ChannelMasks& format,
    const std::uint8_t* data,
    size_t available,
    int width,
    int height,
    const std::string& source)
{
    if (format.bitCount != 8 && format.bitCount != 16 && format.bitCount != 24 && format.bitCount != 32)
    {
        throw std::runtime_error(
            "DDS texture '" + source + "' has an unsupported " + std::to_string(format.bitCount) + "-bit pixel format");
    }
    const size_t bytesPerPixel = format.bitCount / 8;
    // Rows are packed to whole bytes. dwPitchOrLinearSize is not trusted: writers get it wrong.
    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    if (available < rowBytes * static_cast<size_t>(height))
    {
        throw std::runtime_error("DDS texture '" + source + "' is truncated");
    }

    TextureData image;
    image.width = width;
    image.height = height;
    image.channelCount = 4;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t index = 0; index < static_cast<size_t>(width) * static_cast<size_t>(height); ++index)
    {
        const std::uint8_t* texel = data + index * bytesPerPixel;
        std::uint32_t pixel = 0;
        for (size_t byte = 0; byte < bytesPerPixel; ++byte)
        {
            pixel |= static_cast<std::uint32_t>(texel[byte]) << (8 * byte);
        }
        std::uint8_t* out = &image.pixels[index * 4];
        if (format.luminance)
        {
            const std::uint8_t grey = ExtractChannel(pixel, format.masks[0]);
            out[0] = grey;
            out[1] = grey;
            out[2] = grey;
        }
        else
        {
            out[0] = ExtractChannel(pixel, format.masks[0]);
            out[1] = ExtractChannel(pixel, format.masks[1]);
            out[2] = ExtractChannel(pixel, format.masks[2]);
        }
        out[3] = format.masks[3] != 0 ? ExtractChannel(pixel, format.masks[3]) : 255;
    }
    return image;
}

std::string DescribeFourCc(std::uint32_t fourCc)
{
    std::string text;
    for (int index = 0; index < 4; ++index)
    {
        const char character = static_cast<char>((fourCc >> (8 * index)) & 0xFF);
        if (character < 32 || character > 126)
        {
            return std::to_string(fourCc);
        }
        text.push_back(character);
    }
    return "'" + text + "'";
}
}

namespace DdsDecoder
{
bool IsDds(const std::uint8_t* bytes, size_t size)
{
    return bytes != nullptr && size >= kMagicSize + kHeaderSize && std::memcmp(bytes, "DDS ", 4) == 0;
}

TextureData Decode(const std::uint8_t* bytes, size_t size, const std::string& source)
{
    if (!IsDds(bytes, size))
    {
        throw std::runtime_error("'" + source + "' is not a DDS texture");
    }

    const std::uint8_t* header = bytes + kMagicSize;
    const std::uint32_t heightValue = ReadU32(header + 8);
    const std::uint32_t widthValue = ReadU32(header + 12);
    const std::uint32_t pixelFlags = ReadU32(header + 76);
    const std::uint32_t fourCc = ReadU32(header + 80);
    ChannelMasks masks;
    masks.bitCount = ReadU32(header + 84);
    masks.masks = {ReadU32(header + 88), ReadU32(header + 92), ReadU32(header + 96), ReadU32(header + 100)};

    constexpr std::uint32_t kMaxDimension = 16384;
    if (widthValue == 0 || heightValue == 0 || widthValue > kMaxDimension || heightValue > kMaxDimension)
    {
        throw std::runtime_error(
            "DDS texture '" + source + "' has an invalid size " + std::to_string(widthValue) + "x" +
            std::to_string(heightValue));
    }
    const int width = static_cast<int>(widthValue);
    const int height = static_cast<int>(heightValue);

    size_t dataOffset = kMagicSize + kHeaderSize;
    if ((pixelFlags & kPixelFourCc) != 0)
    {
        std::optional<BlockFormat> format;
        std::optional<ChannelMasks> uncompressed;
        if (fourCc == FourCc('D', 'X', '1', '0'))
        {
            if (size < dataOffset + kDx10HeaderSize)
            {
                throw std::runtime_error("DDS texture '" + source + "' is truncated");
            }
            const std::uint32_t dxgi = ReadU32(bytes + dataOffset);
            dataOffset += kDx10HeaderSize;
            switch (dxgi)
            {
            case kDxgiBc1Unorm:
            case kDxgiBc1Srgb:
                format = BlockFormat::Bc1;
                break;
            case kDxgiBc2Unorm:
            case kDxgiBc2Srgb:
                format = BlockFormat::Bc2;
                break;
            case kDxgiBc3Unorm:
            case kDxgiBc3Srgb:
                format = BlockFormat::Bc3;
                break;
            case kDxgiBc4Unorm:
                format = BlockFormat::Bc4;
                break;
            case kDxgiBc5Unorm:
                format = BlockFormat::Bc5;
                break;
            case kDxgiBc7Unorm:
            case kDxgiBc7Srgb:
                format = BlockFormat::Bc7;
                break;
            case kDxgiR8G8B8A8Unorm:
            case kDxgiR8G8B8A8Srgb:
                uncompressed = ChannelMasks{32, {0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u}, false};
                break;
            case kDxgiB8G8R8A8Unorm:
            case kDxgiB8G8R8A8Srgb:
                uncompressed = ChannelMasks{32, {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u}, false};
                break;
            case kDxgiB8G8R8X8Unorm:
            case kDxgiB8G8R8X8Srgb:
                uncompressed = ChannelMasks{32, {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0u}, false};
                break;
            default:
                throw std::runtime_error(
                    "DDS texture '" + source + "' uses unsupported DXGI format " + std::to_string(dxgi));
            }
        }
        else if (fourCc == FourCc('D', 'X', 'T', '1'))
        {
            format = BlockFormat::Bc1;
        }
        else if (fourCc == FourCc('D', 'X', 'T', '2') || fourCc == FourCc('D', 'X', 'T', '3'))
        {
            format = BlockFormat::Bc2;
        }
        else if (fourCc == FourCc('D', 'X', 'T', '4') || fourCc == FourCc('D', 'X', 'T', '5'))
        {
            format = BlockFormat::Bc3;
        }
        else if (fourCc == FourCc('A', 'T', 'I', '1') || fourCc == FourCc('B', 'C', '4', 'U'))
        {
            format = BlockFormat::Bc4;
        }
        else if (fourCc == FourCc('A', 'T', 'I', '2') || fourCc == FourCc('B', 'C', '5', 'U'))
        {
            format = BlockFormat::Bc5;
        }
        else
        {
            throw std::runtime_error(
                "DDS texture '" + source + "' uses unsupported format " + DescribeFourCc(fourCc));
        }

        if (dataOffset > size)
        {
            throw std::runtime_error("DDS texture '" + source + "' is truncated");
        }
        if (format.has_value())
        {
            return DecodeBlocks(*format, bytes + dataOffset, size - dataOffset, width, height, source);
        }
        return DecodeMasked(*uncompressed, bytes + dataOffset, size - dataOffset, width, height, source);
    }

    if ((pixelFlags & (kPixelRgb | kPixelLuminance | kPixelAlpha)) == 0)
    {
        throw std::runtime_error("DDS texture '" + source + "' has an unsupported pixel format");
    }
    masks.luminance = (pixelFlags & kPixelLuminance) != 0;
    if ((pixelFlags & kPixelRgb) == 0 && !masks.luminance)
    {
        // Alpha only: colour white, so the image reads as coverage.
        const std::uint32_t alphaMask = masks.masks[3];
        masks.masks = {0, 0, 0, alphaMask};
        TextureData image = DecodeMasked(masks, bytes + dataOffset, size - dataOffset, width, height, source);
        for (size_t index = 0; index < image.pixels.size(); index += 4)
        {
            image.pixels[index] = 255;
            image.pixels[index + 1] = 255;
            image.pixels[index + 2] = 255;
        }
        return image;
    }
    if ((pixelFlags & (kPixelAlphaPixels | kPixelAlpha)) == 0)
    {
        masks.masks[3] = 0;
    }
    return DecodeMasked(masks, bytes + dataOffset, size - dataOffset, width, height, source);
}
}
}
