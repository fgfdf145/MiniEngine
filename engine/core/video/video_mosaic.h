#pragma once

#include "video_recorder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace me
{

// A quad recording puts its four cameras' images on one canvas
// (docs/design/2026-10-07-quad-vehicle-recording-design.md). They come in the order front, rear,
// left, right.
inline constexpr size_t kVideoMosaicImageCount = 4;

enum class VideoMosaicLayout : uint8_t
{
    // 2 x 2: front and rear on top, left and right below.
    Grid,
    // 3 x 3 with the centre empty: front on top, left and right either side of the middle row, rear
    // below, each where it looks at the car from.
    Cross,
};

struct VideoMosaicSize
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const VideoMosaicSize&) const = default;
};

// Where an image goes on the canvas, top-left corner first, in pixels.
struct VideoMosaicTile
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const VideoMosaicTile&) const = default;
};

struct VideoMosaic
{
    uint32_t width = 0;
    uint32_t height = 0;
    // One per image, in the images' order.
    std::vector<VideoMosaicTile> tiles;
};

// Columns as wide as their widest image and rows as tall as their tallest, each image centred in
// its cell, and the canvas's sides rounded up to even numbers (H.264's 4:2:0 needs pairs).
VideoMosaic ComputeVideoMosaic(VideoMosaicLayout layout, std::span<const VideoMosaicSize, kVideoMosaicImageCount> sizes);

// The size DrawVideoLabel's text takes, its dark box included: a 5 x 7 pixel font, each font pixel
// scale x scale, a font pixel apart, with a font pixel of box all round.
VideoMosaicSize MeasureVideoLabel(std::string_view text, uint32_t scale);

// Writes text in white on a black box with its top-left corner at (x, y), into tightly packed
// pixels of width x height in format, clipped to them. The font has the letters (lowercase drawn as
// uppercase), the digits, the space and - . : / ( ) %; anything else is a blank.
void DrawVideoLabel(
    std::span<uint8_t> pixels,
    VideoPixelFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t x,
    uint32_t y,
    std::string_view text,
    uint32_t scale);
}
