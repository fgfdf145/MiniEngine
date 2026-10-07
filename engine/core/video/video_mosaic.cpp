#include "video_mosaic.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace me
{

namespace
{
constexpr uint32_t kGlyphWidth = 5;
constexpr uint32_t kGlyphHeight = 7;

struct Glyph
{
    char character;
    // Top row first; bit 4 is the leftmost pixel.
    std::array<uint8_t, kGlyphHeight> rows;
};

// A 5 x 7 font: enough for a camera's name and a few figures.
constexpr std::array<Glyph, 43> kGlyphs = {{
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
}};

const Glyph* FindGlyph(char character)
{
    if (character >= 'a' && character <= 'z')
    {
        character = static_cast<char>(character - 'a' + 'A');
    }
    for (const Glyph& glyph : kGlyphs)
    {
        if (glyph.character == character)
        {
            return &glyph;
        }
    }
    return nullptr;
}

uint32_t TexelBytes(VideoPixelFormat format)
{
    return format == VideoPixelFormat::RgbaHalf ? 8u : 4u;
}

// One pixel, white or black, opaque.
void WritePixel(uint8_t* texel, VideoPixelFormat format, bool white)
{
    if (format == VideoPixelFormat::RgbaHalf)
    {
        // Half-float 1.0 is 0x3C00; the alpha is 1 either way.
        const uint16_t value = white ? 0x3C00u : 0u;
        const std::array<uint16_t, 4> halves = {value, value, value, 0x3C00u};
        std::memcpy(texel, halves.data(), sizeof(halves));
        return;
    }
    const uint8_t value = white ? 255u : 0u;
    texel[0] = value;
    texel[1] = value;
    texel[2] = value;
    texel[3] = 255u;
}
}

VideoMosaic ComputeVideoMosaic(VideoMosaicLayout layout, std::span<const VideoMosaicSize, kVideoMosaicImageCount> sizes)
{
    const VideoMosaicSize& front = sizes[0];
    const VideoMosaicSize& rear = sizes[1];
    const VideoMosaicSize& left = sizes[2];
    const VideoMosaicSize& right = sizes[3];
    // An image centred in its cell.
    const auto place = [](const VideoMosaicSize& size, uint32_t cellX, uint32_t cellY, uint32_t cellWidth, uint32_t cellHeight)
    {
        return VideoMosaicTile{cellX + (cellWidth - size.width) / 2, cellY + (cellHeight - size.height) / 2, size.width, size.height};
    };

    VideoMosaic mosaic;
    if (layout == VideoMosaicLayout::Grid)
    {
        const uint32_t column0 = std::max(front.width, left.width);
        const uint32_t column1 = std::max(rear.width, right.width);
        const uint32_t row0 = std::max(front.height, rear.height);
        const uint32_t row1 = std::max(left.height, right.height);
        mosaic.width = column0 + column1;
        mosaic.height = row0 + row1;
        mosaic.tiles = {
            place(front, 0, 0, column0, row0),
            place(rear, column0, 0, column1, row0),
            place(left, 0, row0, column0, row1),
            place(right, column0, row0, column1, row1)};
    }
    else
    {
        const uint32_t column0 = left.width;
        const uint32_t column1 = std::max(front.width, rear.width);
        const uint32_t column2 = right.width;
        const uint32_t row0 = front.height;
        const uint32_t row1 = std::max(left.height, right.height);
        const uint32_t row2 = rear.height;
        mosaic.width = column0 + column1 + column2;
        mosaic.height = row0 + row1 + row2;
        mosaic.tiles = {
            place(front, column0, 0, column1, row0),
            place(rear, column0, row0 + row1, column1, row2),
            place(left, 0, row0, column0, row1),
            place(right, column0 + column1, row0, column2, row1)};
    }
    mosaic.width += mosaic.width % 2;
    mosaic.height += mosaic.height % 2;
    return mosaic;
}

VideoMosaicSize MeasureVideoLabel(std::string_view text, uint32_t scale)
{
    if (text.empty() || scale == 0)
    {
        return {};
    }
    const uint32_t characters = static_cast<uint32_t>(text.size());
    // A font pixel of box, then each glyph and a font pixel between glyphs, then a font pixel of box.
    return VideoMosaicSize{(characters * (kGlyphWidth + 1) + 1) * scale, (kGlyphHeight + 2) * scale};
}

void DrawVideoLabel(
    std::span<uint8_t> pixels,
    VideoPixelFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t x,
    uint32_t y,
    std::string_view text,
    uint32_t scale)
{
    const uint32_t texelBytes = TexelBytes(format);
    if (pixels.size() < static_cast<size_t>(width) * height * texelBytes)
    {
        return;
    }
    const VideoMosaicSize box = MeasureVideoLabel(text, scale);
    const auto texelAt = [&](uint32_t px, uint32_t py)
    {
        return pixels.data() + (static_cast<size_t>(py) * width + px) * texelBytes;
    };
    for (uint32_t py = y; py < std::min(y + box.height, height); ++py)
    {
        for (uint32_t px = x; px < std::min(x + box.width, width); ++px)
        {
            WritePixel(texelAt(px, py), format, false);
        }
    }
    for (size_t index = 0; index < text.size(); ++index)
    {
        const Glyph* glyph = FindGlyph(text[index]);
        if (glyph == nullptr)
        {
            continue;
        }
        const uint32_t glyphX = x + (static_cast<uint32_t>(index) * (kGlyphWidth + 1) + 1) * scale;
        const uint32_t glyphY = y + scale;
        for (uint32_t row = 0; row < kGlyphHeight; ++row)
        {
            for (uint32_t column = 0; column < kGlyphWidth; ++column)
            {
                if ((glyph->rows[row] & (0x10u >> column)) == 0)
                {
                    continue;
                }
                for (uint32_t dy = 0; dy < scale; ++dy)
                {
                    for (uint32_t dx = 0; dx < scale; ++dx)
                    {
                        const uint32_t px = glyphX + column * scale + dx;
                        const uint32_t py = glyphY + row * scale + dy;
                        if (px < width && py < height)
                        {
                            WritePixel(texelAt(px, py), format, true);
                        }
                    }
                }
            }
        }
    }
}
}
