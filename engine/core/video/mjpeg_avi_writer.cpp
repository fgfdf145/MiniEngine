#include "mjpeg_avi_writer.h"

#include <algorithm>
#include <array>

namespace me
{

namespace
{
// The AVI headers, laid out once by Open and patched by Close (see the offsets below).
// RIFF 'AVI '
//   LIST 'hdrl'
//     'avih' MainAVIHeader
//     LIST 'strl'
//       'strh' AVIStreamHeader
//       'strf' BITMAPINFOHEADER
//   LIST 'movi'
//     '00dc' JPEG ...
//   'idx1' AVIINDEXENTRY ...
constexpr uint64_t kRiffSizeOffset = 4;
constexpr uint64_t kAvihMaxBytesPerSecondOffset = 36;
constexpr uint64_t kAvihTotalFramesOffset = 48;
constexpr uint64_t kAvihSuggestedBufferOffset = 60;
constexpr uint64_t kStrhLengthOffset = 140;
constexpr uint64_t kStrhSuggestedBufferOffset = 144;
constexpr uint64_t kMoviSizeOffset = 216;
constexpr uint64_t kHeaderBytes = 224;

constexpr uint32_t kAvifHasIndex = 0x10;
constexpr uint32_t kAviifKeyframe = 0x10;

class ByteWriter
{
  public:
    void FourCc(const char (&code)[5])
    {
        m_bytes.insert(m_bytes.end(), code, code + 4);
    }
    void U32(uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            m_bytes.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    void U16(uint16_t value)
    {
        m_bytes.push_back(static_cast<uint8_t>(value));
        m_bytes.push_back(static_cast<uint8_t>(value >> 8));
    }
    const std::vector<uint8_t>& Bytes() const
    {
        return m_bytes;
    }

  private:
    std::vector<uint8_t> m_bytes;
};

std::array<char, 4> LittleEndian(uint32_t value)
{
    return {
        static_cast<char>(value),
        static_cast<char>(value >> 8),
        static_cast<char>(value >> 16),
        static_cast<char>(value >> 24)};
}
}

MjpegAviWriter::~MjpegAviWriter()
{
    std::string ignored;
    Close(ignored);
}

bool MjpegAviWriter::Open(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    uint32_t framesPerSecond,
    std::string& error)
{
    if (IsOpen())
    {
        error = "The video file is already open";
        return false;
    }
    if (width == 0 || height == 0 || width > 0xFFFF || height > 0xFFFF || framesPerSecond == 0)
    {
        error = "Invalid video size or frame rate";
        return false;
    }
    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file)
    {
        error = "Cannot create '" + path.string() + "'";
        return false;
    }
    m_path = path;
    m_width = width;
    m_height = height;
    m_framesPerSecond = framesPerSecond;
    m_index.clear();
    m_largestFrame = 0;

    ByteWriter header;
    header.FourCc("RIFF");
    header.U32(0); // patched by Close
    header.FourCc("AVI ");

    header.FourCc("LIST");
    header.U32(192);
    header.FourCc("hdrl");
    header.FourCc("avih");
    header.U32(56);
    header.U32(1'000'000u / framesPerSecond); // microseconds per frame
    header.U32(0);                            // max bytes per second, patched
    header.U32(0);                            // padding granularity
    header.U32(kAvifHasIndex);
    header.U32(0); // total frames, patched
    header.U32(0); // initial frames
    header.U32(1); // streams
    header.U32(0); // suggested buffer size, patched
    header.U32(width);
    header.U32(height);
    for (int reserved = 0; reserved < 4; ++reserved)
    {
        header.U32(0);
    }

    header.FourCc("LIST");
    header.U32(116);
    header.FourCc("strl");
    header.FourCc("strh");
    header.U32(56);
    header.FourCc("vids");
    header.FourCc("MJPG");
    header.U32(0); // flags
    header.U16(0); // priority
    header.U16(0); // language
    header.U32(0); // initial frames
    header.U32(1); // scale: rate / scale is the frame rate
    header.U32(framesPerSecond);
    header.U32(0);            // start
    header.U32(0);            // length in frames, patched
    header.U32(0);            // suggested buffer size, patched
    header.U32(0xFFFF'FFFFu); // quality: the default
    header.U32(0);            // sample size: frames vary in size
    header.U16(0);
    header.U16(0);
    header.U16(static_cast<uint16_t>(width));
    header.U16(static_cast<uint16_t>(height));
    header.FourCc("strf");
    header.U32(40);
    header.U32(40); // BITMAPINFOHEADER size
    header.U32(width);
    header.U32(height);
    header.U16(1);  // planes
    header.U16(24); // bits per pixel once decoded
    header.FourCc("MJPG");
    header.U32(width * height * 3);
    header.U32(0);
    header.U32(0);
    header.U32(0);
    header.U32(0);

    header.FourCc("LIST");
    header.U32(4); // patched by Close
    header.FourCc("movi");

    const std::vector<uint8_t>& bytes = header.Bytes();
    m_file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    m_bytesWritten = bytes.size();
    m_moviFourccOffset = kMoviSizeOffset + 4;
    if (!m_file || bytes.size() != kHeaderBytes)
    {
        error = "Cannot write '" + path.string() + "'";
        m_file.close();
        return false;
    }
    return true;
}

bool MjpegAviWriter::WriteFrame(std::span<const uint8_t> jpeg, std::string& error)
{
    if (!IsOpen())
    {
        error = "The video file is not open";
        return false;
    }
    if (!CanFit(jpeg.size()))
    {
        error = "The video file is full";
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(jpeg.size());
    m_index.push_back({static_cast<uint32_t>(m_bytesWritten - m_moviFourccOffset), size});
    m_largestFrame = std::max(m_largestFrame, size);

    m_file.write("00dc", 4);
    m_file.write(LittleEndian(size).data(), 4);
    m_file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    m_bytesWritten += 8 + jpeg.size();
    // Every chunk starts on an even byte.
    if ((jpeg.size() & 1) != 0)
    {
        m_file.put('\0');
        ++m_bytesWritten;
    }
    if (!m_file)
    {
        error = "Cannot write '" + m_path.string() + "' (is the disk full?)";
        return false;
    }
    return true;
}

bool MjpegAviWriter::Close(std::string& error)
{
    if (!IsOpen())
    {
        return true;
    }
    const uint64_t moviEnd = m_bytesWritten;

    ByteWriter index;
    index.FourCc("idx1");
    index.U32(static_cast<uint32_t>(16 * m_index.size()));
    for (const IndexEntry& entry : m_index)
    {
        index.FourCc("00dc");
        index.U32(kAviifKeyframe);
        index.U32(entry.offset);
        index.U32(entry.size);
    }
    const std::vector<uint8_t>& indexBytes = index.Bytes();
    m_file.write(reinterpret_cast<const char*>(indexBytes.data()), static_cast<std::streamsize>(indexBytes.size()));
    m_bytesWritten += indexBytes.size();

    const auto patch = [this](uint64_t offset, uint32_t value)
    {
        m_file.seekp(static_cast<std::streamoff>(offset));
        m_file.write(LittleEndian(value).data(), 4);
    };
    const uint32_t frames = static_cast<uint32_t>(m_index.size());
    const uint32_t suggestedBuffer = m_largestFrame + 8;
    const uint64_t maxBytesPerSecond = static_cast<uint64_t>(suggestedBuffer) * m_framesPerSecond;
    patch(kRiffSizeOffset, static_cast<uint32_t>(m_bytesWritten - 8));
    patch(kAvihMaxBytesPerSecondOffset, static_cast<uint32_t>(std::min<uint64_t>(maxBytesPerSecond, 0xFFFF'FFFFu)));
    patch(kAvihTotalFramesOffset, frames);
    patch(kAvihSuggestedBufferOffset, suggestedBuffer);
    patch(kStrhLengthOffset, frames);
    patch(kStrhSuggestedBufferOffset, suggestedBuffer);
    patch(kMoviSizeOffset, static_cast<uint32_t>(moviEnd - m_moviFourccOffset));

    const bool written = static_cast<bool>(m_file);
    m_file.close();
    m_index.clear();
    if (!written || m_file.fail())
    {
        error = "Cannot finish '" + m_path.string() + "' (is the disk full?)";
        return false;
    }
    return true;
}
}
