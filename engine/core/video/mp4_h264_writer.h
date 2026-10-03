#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace me
{

// Moves an MP4's moov box (its index) from the end to just before the media data, the "fast start"
// layout a browser can play while it downloads, and shifts the chunk offsets in it to match. A file
// already laid out so is left alone. Rewrites the file through <path>.faststart; returns false and
// fills error, leaving the file as it was, when it cannot (no moov or mdat, or 32-bit offsets that
// would overflow).
bool MoveMp4IndexToFront(const std::filesystem::path& path, std::string& error);

// Writes H.264 video into an MP4 file through Windows Media Foundation's sink writer, which picks
// the GPU's hardware encoder when there is one. The format sites like Reddit and YouTube take as is:
// High profile, 4:2:0, BT.709 limited range (tagged in the stream), the moov box moved before the
// media data on Close (MoveMp4IndexToFront). One video stream, no audio, a
// constant frame rate. Windows only: elsewhere Open fails.
class Mp4H264Writer
{
  public:
    Mp4H264Writer();
    Mp4H264Writer(const Mp4H264Writer&) = delete;
    Mp4H264Writer& operator=(const Mp4H264Writer&) = delete;
    ~Mp4H264Writer();

    // Whether this build can write MP4 at all.
    static bool IsSupported();
    // A bit rate that keeps detail through a site's own re-encode: 0.2 bits a pixel a frame, from
    // 4 to 120 Mbit/s.
    static uint32_t DefaultBitsPerSecond(uint32_t width, uint32_t height, uint32_t framesPerSecond);

    // Creates the file and the encoder. Width and height must be even. Returns false and fills error
    // on failure.
    bool Open(
        const std::filesystem::path& path,
        uint32_t width,
        uint32_t height,
        uint32_t framesPerSecond,
        uint32_t bitsPerSecond,
        std::string& error);
    // Appends one frame: NV12 (a width x height luma plane, then interleaved half-size Cb Cr), BT.709
    // limited range, as ConvertVideoFrameToNv12 makes it. May be called from any thread, one at a time.
    bool WriteFrame(std::span<const uint8_t> nv12, std::string& error);
    // Drains the encoder and finishes the file; it plays only once this has run. Runs from the
    // destructor too.
    bool Close(std::string& error);

    bool IsOpen() const;
    uint32_t GetFrameCount() const
    {
        return m_frameCount;
    }
    // The bytes written so far: the encoder's output while open, the file's size once closed.
    uint64_t GetFileBytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::filesystem::path m_path;
    uint32_t m_frameCount = 0;
    uint64_t m_closedFileBytes = 0;
};
}
