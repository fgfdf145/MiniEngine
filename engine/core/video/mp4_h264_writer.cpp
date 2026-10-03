#include "mp4_h264_writer.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <spdlog/fmt/fmt.h>
#endif

namespace me
{

namespace
{
struct Mp4Box
{
    char type[4] = {};
    uint64_t offset = 0;
    uint64_t size = 0; // header included
};

uint32_t ReadBigEndian32(const uint8_t* bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

uint64_t ReadBigEndian64(const uint8_t* bytes)
{
    return (static_cast<uint64_t>(ReadBigEndian32(bytes)) << 32) | ReadBigEndian32(bytes + 4);
}

void WriteBigEndian32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

bool IsType(const Mp4Box& box, const char* type)
{
    return std::memcmp(box.type, type, 4) == 0;
}

// The boxes in bytes [begin, end): a 32-bit size, a 64-bit one after the type when it is 1, or to
// the end when it is 0.
bool ReadBoxes(std::istream& file, uint64_t begin, uint64_t end, std::vector<Mp4Box>& boxes)
{
    for (uint64_t offset = begin; offset + 8 <= end;)
    {
        uint8_t header[16] = {};
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file.read(reinterpret_cast<char*>(header), 8))
        {
            return false;
        }
        Mp4Box box;
        std::memcpy(box.type, header + 4, 4);
        box.offset = offset;
        box.size = ReadBigEndian32(header);
        if (box.size == 1)
        {
            if (!file.read(reinterpret_cast<char*>(header + 8), 8))
            {
                return false;
            }
            box.size = ReadBigEndian64(header + 8);
        }
        else if (box.size == 0)
        {
            box.size = end - offset;
        }
        if (box.size < 8 || offset + box.size > end)
        {
            return false;
        }
        boxes.push_back(box);
        offset += box.size;
    }
    return true;
}

// Adds shift to every chunk offset (stco, co64) in the boxes of moov's bytes [begin, end).
bool ShiftChunkOffsets(std::vector<uint8_t>& moov, size_t begin, size_t end, uint64_t shift)
{
    static constexpr const char* kContainers[] = {"moov", "trak", "mdia", "minf", "stbl"};
    for (size_t offset = begin; offset + 8 <= end;)
    {
        const uint32_t size = ReadBigEndian32(moov.data() + offset);
        const char* type = reinterpret_cast<const char*>(moov.data() + offset + 4);
        // The sink writer writes 32-bit box sizes inside moov.
        if (size < 8 || offset + size > end)
        {
            return false;
        }
        const bool container = std::any_of(std::begin(kContainers), std::end(kContainers), [type](const char* name)
                                           {
                                               return std::memcmp(type, name, 4) == 0;
                                           });
        const bool stco = std::memcmp(type, "stco", 4) == 0;
        const bool co64 = std::memcmp(type, "co64", 4) == 0;
        if (container && !ShiftChunkOffsets(moov, offset + 8, offset + size, shift))
        {
            return false;
        }
        if (stco || co64)
        {
            // Version and flags, the entry count, the entries.
            if (size < 16)
            {
                return false;
            }
            const uint32_t count = ReadBigEndian32(moov.data() + offset + 12);
            const size_t entryBytes = co64 ? 8 : 4;
            if (16 + static_cast<uint64_t>(count) * entryBytes > size)
            {
                return false;
            }
            for (uint32_t entry = 0; entry < count; ++entry)
            {
                uint8_t* field = moov.data() + offset + 16 + entry * entryBytes;
                if (co64)
                {
                    const uint64_t value = ReadBigEndian64(field) + shift;
                    WriteBigEndian32(field, static_cast<uint32_t>(value >> 32));
                    WriteBigEndian32(field + 4, static_cast<uint32_t>(value));
                }
                else
                {
                    const uint64_t value = static_cast<uint64_t>(ReadBigEndian32(field)) + shift;
                    if (value > 0xFFFF'FFFFull)
                    {
                        return false;
                    }
                    WriteBigEndian32(field, static_cast<uint32_t>(value));
                }
            }
        }
        offset += size;
    }
    return true;
}

bool CopyBytes(std::istream& from, std::ostream& to, uint64_t offset, uint64_t count)
{
    std::vector<char> buffer(static_cast<size_t>(std::min<uint64_t>(count, 8ull << 20)));
    from.seekg(static_cast<std::streamoff>(offset));
    while (count > 0)
    {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(count, buffer.size()));
        if (!from.read(buffer.data(), static_cast<std::streamsize>(chunk)) || !to.write(buffer.data(), static_cast<std::streamsize>(chunk)))
        {
            return false;
        }
        count -= chunk;
    }
    return true;
}
}

bool MoveMp4IndexToFront(const std::filesystem::path& path, std::string& error)
{
    std::error_code sizeError;
    const uint64_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        error = "The MP4 file cannot be read: " + sizeError.message();
        return false;
    }
    const std::filesystem::path rewritten = path.string() + ".faststart";
    {
        std::ifstream input(path, std::ios::binary);
        std::vector<Mp4Box> boxes;
        if (!input || !ReadBoxes(input, 0, fileSize, boxes))
        {
            error = "The MP4 file's boxes do not parse";
            return false;
        }
        const auto find = [&boxes](const char* type)
        {
            return std::find_if(boxes.begin(), boxes.end(), [type](const Mp4Box& box)
                                {
                                    return IsType(box, type);
                                });
        };
        const auto moovBox = find("moov");
        const auto mdatBox = find("mdat");
        if (moovBox == boxes.end() || mdatBox == boxes.end())
        {
            error = "The MP4 file has no moov or no mdat box";
            return false;
        }
        if (moovBox->offset < mdatBox->offset)
        {
            return true;
        }
        const Mp4Box moovEntry = *moovBox;
        const Mp4Box mdatEntry = *mdatBox;
        if (moovEntry.size > (256ull << 20))
        {
            error = "The MP4 file's moov box is too large to move";
            return false;
        }
        std::vector<uint8_t> moov(static_cast<size_t>(moovEntry.size));
        input.seekg(static_cast<std::streamoff>(moovEntry.offset));
        if (!input.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(moov.size())) ||
            ReadBigEndian32(moov.data()) != moovEntry.size ||
            !ShiftChunkOffsets(moov, 8, moov.size(), moovEntry.size))
        {
            error = "The MP4 file's chunk offsets cannot be moved";
            return false;
        }

        // What came before mdat, the moov box, then mdat and whatever was between it and moov.
        std::ofstream output(rewritten, std::ios::binary | std::ios::trunc);
        const bool copied =
            output &&
            CopyBytes(input, output, 0, mdatEntry.offset) &&
            output.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size())) &&
            CopyBytes(input, output, mdatEntry.offset, moovEntry.offset - mdatEntry.offset) &&
            CopyBytes(input, output, moovEntry.offset + moovEntry.size, fileSize - moovEntry.offset - moovEntry.size);
        output.close();
        if (!copied || !output)
        {
            std::filesystem::remove(rewritten, sizeError);
            error = "Failed to rewrite the MP4 file";
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(rewritten, path, renameError);
    if (renameError)
    {
        std::filesystem::remove(rewritten, sizeError);
        error = "Failed to replace the MP4 file: " + renameError.message();
        return false;
    }
    return true;
}

uint32_t Mp4H264Writer::DefaultBitsPerSecond(uint32_t width, uint32_t height, uint32_t framesPerSecond)
{
    const double bits = 0.2 * width * height * framesPerSecond;
    return static_cast<uint32_t>(std::clamp(bits, 4.0e6, 120.0e6));
}

#ifdef _WIN32

namespace
{
using Microsoft::WRL::ComPtr;

std::string Describe(const char* what, HRESULT result)
{
    return fmt::format("{} (HRESULT 0x{:08X})", what, static_cast<uint32_t>(result));
}

// Media Foundation runs in the multithreaded apartment; a thread that has not joined one joins it
// here. A thread already in a single-threaded one stays there: the sink writer is free-threaded.
void JoinComApartment()
{
    thread_local bool joined = false;
    if (!joined)
    {
        joined = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
}

// What the frames are: progressive, square pixels, BT.709 limited range. On the input the encoder
// converts nothing by it; on the output it becomes the stream's VUI, which tells players how to
// turn the YUV back into the colours the viewport showed.
HRESULT SetVideoAttributes(IMFMediaType* type, uint32_t width, uint32_t height, uint32_t framesPerSecond)
{
    HRESULT result = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(result))
        result = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(result))
        result = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, framesPerSecond, 1);
    if (SUCCEEDED(result))
        result = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED(result))
        result = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    return result;
}
}

struct Mp4H264Writer::Impl
{
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framesPerSecond = 0;
    bool started = false;
};

Mp4H264Writer::Mp4H264Writer() = default;

Mp4H264Writer::~Mp4H264Writer()
{
    std::string error;
    Close(error);
}

bool Mp4H264Writer::IsSupported()
{
    return true;
}

bool Mp4H264Writer::Open(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    uint32_t framesPerSecond,
    uint32_t bitsPerSecond,
    std::string& error)
{
    if (IsOpen())
    {
        error = "The MP4 file is already open";
        return false;
    }
    if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0 || framesPerSecond == 0)
    {
        error = fmt::format("An MP4 needs an even size and a frame rate, not {}x{} at {}", width, height, framesPerSecond);
        return false;
    }
    JoinComApartment();
    HRESULT result = MFStartup(MF_VERSION);
    if (FAILED(result))
    {
        error = Describe("Media Foundation did not start", result);
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->started = true;
    impl->width = width;
    impl->height = height;
    impl->framesPerSecond = framesPerSecond;
    m_path = path;
    m_frameCount = 0;
    m_closedFileBytes = 0;

    // From here a failure shuts Media Foundation down with impl.
    const auto fail = [&](const char* what)
    {
        error = Describe(what, result);
        impl->writer.Reset();
        MFShutdown();
        return false;
    };

    ComPtr<IMFAttributes> attributes;
    result = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(result))
        result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    // EveryFrame recordings hand frames over as fast as they render; the queue is VideoRecorder's.
    if (SUCCEEDED(result))
        result = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED(result))
        result = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (FAILED(result))
        return fail("Failed to set up the MP4 writer");
    result = MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, attributes.Get(), &impl->writer);
    if (FAILED(result))
        return fail("Failed to create the MP4 file");

    ComPtr<IMFMediaType> output;
    result = MFCreateMediaType(&output);
    if (SUCCEEDED(result))
        result = SetVideoAttributes(output.Get(), width, height, framesPerSecond);
    if (SUCCEEDED(result))
        result = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(result))
        result = output->SetUINT32(MF_MT_AVG_BITRATE, bitsPerSecond);
    if (SUCCEEDED(result))
        result = output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(result))
        result = impl->writer->AddStream(output.Get(), &impl->stream);
    if (FAILED(result))
        return fail("No H.264 encoder takes this size and frame rate");

    ComPtr<IMFMediaType> input;
    result = MFCreateMediaType(&input);
    if (SUCCEEDED(result))
        result = SetVideoAttributes(input.Get(), width, height, framesPerSecond);
    if (SUCCEEDED(result))
        result = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(result))
        result = input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (SUCCEEDED(result))
        result = impl->writer->SetInputMediaType(impl->stream, input.Get(), nullptr);
    if (FAILED(result))
        return fail("The H.264 encoder does not take NV12 frames of this size");

    result = impl->writer->BeginWriting();
    if (FAILED(result))
        return fail("Failed to start writing the MP4 file");
    m_impl = std::move(impl);
    return true;
}

bool Mp4H264Writer::WriteFrame(std::span<const uint8_t> nv12, std::string& error)
{
    if (!IsOpen())
    {
        error = "The MP4 file is not open";
        return false;
    }
    const size_t expected = static_cast<size_t>(m_impl->width) * m_impl->height * 3 / 2;
    if (nv12.size() != expected)
    {
        error = fmt::format("An NV12 frame of {} bytes, not {}", nv12.size(), expected);
        return false;
    }
    JoinComApartment();

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateMemoryBuffer(static_cast<DWORD>(expected), &buffer);
    if (SUCCEEDED(result))
    {
        BYTE* data = nullptr;
        result = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(result))
        {
            std::memcpy(data, nv12.data(), expected);
            buffer->Unlock();
            result = buffer->SetCurrentLength(static_cast<DWORD>(expected));
        }
    }
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(result))
        result = MFCreateSample(&sample);
    if (SUCCEEDED(result))
        result = sample->AddBuffer(buffer.Get());
    // In 100 ns units, from the frame number so the timestamps do not drift at 30 or 60 fps.
    const LONGLONG start = static_cast<LONGLONG>(m_frameCount) * 10'000'000 / m_impl->framesPerSecond;
    const LONGLONG end = static_cast<LONGLONG>(m_frameCount + 1) * 10'000'000 / m_impl->framesPerSecond;
    if (SUCCEEDED(result))
        result = sample->SetSampleTime(start);
    if (SUCCEEDED(result))
        result = sample->SetSampleDuration(end - start);
    if (SUCCEEDED(result))
        result = m_impl->writer->WriteSample(m_impl->stream, sample.Get());
    if (FAILED(result))
    {
        error = Describe("Failed to encode a frame into the MP4 file", result);
        return false;
    }
    ++m_frameCount;
    return true;
}

bool Mp4H264Writer::Close(std::string& error)
{
    if (!m_impl)
    {
        return true;
    }
    JoinComApartment();
    HRESULT result = S_OK;
    if (m_impl->writer)
    {
        result = m_impl->writer->Finalize();
        m_impl->writer.Reset();
    }
    m_impl.reset();
    MFShutdown();
    if (SUCCEEDED(result))
    {
        // Media Foundation writes the index last. Should moving it fail the file still plays, only
        // not before it has downloaded.
        std::string moveError;
        MoveMp4IndexToFront(m_path, moveError);
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(m_path, sizeError);
    m_closedFileBytes = sizeError ? 0 : size;
    if (FAILED(result))
    {
        error = Describe("Failed to finish the MP4 file", result);
        return false;
    }
    return true;
}

bool Mp4H264Writer::IsOpen() const
{
    return m_impl != nullptr && m_impl->writer != nullptr;
}

uint64_t Mp4H264Writer::GetFileBytes() const
{
    if (!IsOpen())
    {
        return m_closedFileBytes;
    }
    // The sink writes the media data as the encoder hands it over (the sink writer's statistics
    // count only what reached the sink object, a few hundred bytes until Finalize).
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(m_path, sizeError);
    return sizeError ? 0 : size;
}

#else

struct Mp4H264Writer::Impl
{
};

Mp4H264Writer::Mp4H264Writer() = default;
Mp4H264Writer::~Mp4H264Writer() = default;

bool Mp4H264Writer::IsSupported()
{
    return false;
}

bool Mp4H264Writer::Open(const std::filesystem::path&, uint32_t, uint32_t, uint32_t, uint32_t, std::string& error)
{
    error = "MP4 recording needs Windows Media Foundation; record an .avi instead";
    return false;
}

bool Mp4H264Writer::WriteFrame(std::span<const uint8_t>, std::string& error)
{
    error = "The MP4 file is not open";
    return false;
}

bool Mp4H264Writer::Close(std::string&)
{
    return true;
}

bool Mp4H264Writer::IsOpen() const
{
    return false;
}

uint64_t Mp4H264Writer::GetFileBytes() const
{
    return m_closedFileBytes;
}

#endif
}
