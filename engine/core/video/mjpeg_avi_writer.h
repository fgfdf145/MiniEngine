#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace me
{

// Writes Motion JPEG video into an AVI file: every frame its own JPEG, which every player and editor
// reads (ffmpeg, VLC, Windows' Films & TV, Premiere, Resolve) and which needs no codec library.
// One video stream, no audio, a constant frame rate. A classic AVI with an idx1 index: its sizes and
// offsets are 32-bit, so a file stays below kMaxFileBytes and VideoRecorder starts a new one there.
class MjpegAviWriter
{
  public:
    // Past this the 32-bit RIFF size and index offsets would overflow; kept well under 4 GiB.
    static constexpr uint64_t kMaxFileBytes = 0xF000'0000ull;

    MjpegAviWriter() = default;
    MjpegAviWriter(const MjpegAviWriter&) = delete;
    MjpegAviWriter& operator=(const MjpegAviWriter&) = delete;
    ~MjpegAviWriter();

    // Creates the file and writes its headers. Returns false and fills error on failure.
    bool Open(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t framesPerSecond, std::string& error);
    // Appends one frame: a complete JPEG (SOI to EOI) of width x height.
    bool WriteFrame(std::span<const uint8_t> jpeg, std::string& error);
    // Writes the index and the final sizes. The file plays only once this has run; it runs from the
    // destructor too.
    bool Close(std::string& error);

    bool IsOpen() const
    {
        return m_file.is_open();
    }
    uint32_t GetFrameCount() const
    {
        return static_cast<uint32_t>(m_index.size());
    }
    // The file's size once Close has added the index, were it closed now.
    uint64_t GetProjectedFileBytes() const
    {
        return m_bytesWritten + 8 + 16ull * m_index.size();
    }
    // Whether a frame of this many bytes still fits under the size limit.
    bool CanFit(size_t jpegBytes) const
    {
        return GetProjectedFileBytes() + 8 + jpegBytes + 1 + 16 <= m_maxFileBytes;
    }
    // Lowers the size limit (for tests); kMaxFileBytes at most.
    void SetMaxFileBytes(uint64_t maxFileBytes)
    {
        m_maxFileBytes = maxFileBytes < kMaxFileBytes ? maxFileBytes : kMaxFileBytes;
    }

  private:
    struct IndexEntry
    {
        uint32_t offset = 0; // from the "movi" fourcc
        uint32_t size = 0;
    };

    std::ofstream m_file;
    std::filesystem::path m_path;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_framesPerSecond = 0;
    uint64_t m_maxFileBytes = kMaxFileBytes;
    uint64_t m_bytesWritten = 0;
    // Where the "movi" fourcc is: chunk offsets are counted from it.
    uint64_t m_moviFourccOffset = 0;
    uint32_t m_largestFrame = 0;
    std::vector<IndexEntry> m_index;
};
}
