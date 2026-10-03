#include <engine/core/video/mjpeg_avi_writer.h>
#include <engine/core/video/video_recorder.h>

// The test reads the JPEGs back; engine_core links no image reader.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

uint32_t U32(const std::vector<uint8_t>& bytes, size_t offset)
{
    Require(offset + 4 <= bytes.size(), "a read past the end of the file");
    return bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool FourCcAt(const std::vector<uint8_t>& bytes, size_t offset, const char* code)
{
    return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, code, 4) == 0;
}

// What a player reads from an AVI: its frame count, and each frame through the index.
struct ParsedAvi
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t microsecondsPerFrame = 0;
    uint32_t totalFrames = 0;
    std::vector<std::vector<uint8_t>> frames;
};

ParsedAvi ParseAvi(const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes = ReadFile(path);
    Require(FourCcAt(bytes, 0, "RIFF") && FourCcAt(bytes, 8, "AVI "), "the file is a RIFF AVI");
    Require(U32(bytes, 4) + 8 == bytes.size(), "the RIFF size is the file's");
    Require(FourCcAt(bytes, 12, "LIST") && FourCcAt(bytes, 20, "hdrl") && FourCcAt(bytes, 24, "avih"), "the header list comes first");

    ParsedAvi avi;
    avi.microsecondsPerFrame = U32(bytes, 32);
    avi.totalFrames = U32(bytes, 48);
    avi.width = U32(bytes, 64);
    avi.height = U32(bytes, 68);
    Require(FourCcAt(bytes, 112, "MJPG") && FourCcAt(bytes, 188, "MJPG"), "the stream is Motion JPEG");
    Require(U32(bytes, 140) == avi.totalFrames, "the stream's length is the file's frame count");

    // The movi list, then the index right after it.
    size_t offset = 12 + 8 + U32(bytes, 16);
    Require(FourCcAt(bytes, offset, "LIST") && FourCcAt(bytes, offset + 8, "movi"), "the movi list follows the headers");
    const size_t moviFourcc = offset + 8;
    offset = moviFourcc + U32(bytes, offset + 4);
    Require(FourCcAt(bytes, offset, "idx1"), "the index follows the movi list");
    const uint32_t entries = U32(bytes, offset + 4) / 16;
    Require(entries == avi.totalFrames, "the index has an entry per frame");
    for (uint32_t entry = 0; entry < entries; ++entry)
    {
        const size_t record = offset + 8 + 16 * entry;
        const size_t chunk = moviFourcc + U32(bytes, record + 8);
        const uint32_t size = U32(bytes, record + 12);
        Require(FourCcAt(bytes, record, "00dc") && FourCcAt(bytes, chunk, "00dc"), "each index entry points at a frame chunk");
        Require(U32(bytes, chunk + 4) == size, "each index entry has its chunk's size");
        avi.frames.emplace_back(bytes.begin() + chunk + 8, bytes.begin() + chunk + 8 + size);
    }
    return avi;
}

// Decodes a JPEG frame to RGB and returns the pixel at (x, y).
std::array<int, 3> DecodePixel(const std::vector<uint8_t>& jpeg, int x, int y, int expectedWidth, int expectedHeight)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &width, &height, &channels, 3);
    Require(pixels != nullptr, "each frame is a JPEG");
    Require(width == expectedWidth && height == expectedHeight, "each frame has the video's size");
    const stbi_uc* pixel = pixels + (static_cast<size_t>(y) * width + x) * 3;
    const std::array<int, 3> rgb = {pixel[0], pixel[1], pixel[2]};
    stbi_image_free(pixels);
    return rgb;
}

bool Near(const std::array<int, 3>& rgb, int r, int g, int b)
{
    return std::abs(rgb[0] - r) <= 6 && std::abs(rgb[1] - g) <= 6 && std::abs(rgb[2] - b) <= 6;
}

// A width x height RGBA8 frame, its left half one colour and its right half another.
VideoFrame SplitFrame(uint32_t width, uint32_t height, std::array<uint8_t, 3> left, std::array<uint8_t, 3> right, double time)
{
    VideoFrame frame;
    frame.format = VideoPixelFormat::Rgba8;
    frame.timeSeconds = time;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const std::array<uint8_t, 3>& colour = x < width / 2 ? left : right;
            uint8_t* pixel = frame.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            pixel[0] = colour[0];
            pixel[1] = colour[1];
            pixel[2] = colour[2];
            pixel[3] = 0; // alpha is not the video's: it must not matter
        }
    }
    return frame;
}

std::filesystem::path TempPath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void TestFramesStartedBy()
{
    Require(VideoFramesStartedBy(0.0, 30) == 1, "the first frame starts at once");
    Require(VideoFramesStartedBy(-1.0, 30) == 1, "a time before the first frame is the first frame");
    Require(VideoFramesStartedBy(1.0 / 30.0 - 0.001, 30) == 1, "a frame lasts 1/fps");
    Require(VideoFramesStartedBy(1.0 / 30.0, 30) == 2, "the second frame starts at 1/fps");
    Require(VideoFramesStartedBy(1.0, 30) == 31, "thirty frames last a second");
    Require(VideoFramesStartedBy(0.5, 60) == 31, "the rate decides");
}

void TestPixelConversion()
{
    VideoFrame bgra;
    bgra.format = VideoPixelFormat::Bgra8;
    bgra.pixels = {10, 20, 30, 0};
    const std::vector<uint8_t> fromBgra = ConvertVideoFrameToRgba8(bgra, 1, 1);
    Require(fromBgra == std::vector<uint8_t>{30, 20, 10, 255}, "BGRA comes out as RGBA, opaque");

    // Half floats: 1.0 (0x3C00), 0.5 (0x3800), 0 and 2.0 (0x4000, clipped).
    VideoFrame half;
    half.format = VideoPixelFormat::RgbaHalf;
    const uint16_t halves[4] = {0x3C00, 0x3800, 0x0000, 0x4000};
    half.pixels.resize(sizeof(halves));
    std::memcpy(half.pixels.data(), halves, sizeof(halves));
    const std::vector<uint8_t> fromHalf = ConvertVideoFrameToRgba8(half, 1, 1);
    Require(fromHalf[0] == 255 && fromHalf[2] == 0 && fromHalf[3] == 255, "linear 1 is white and 0 black, alpha opaque");
    Require(fromHalf[1] == 188, "linear 0.5 is sRGB-encoded");
}

void TestWriterProducesAPlayableAvi()
{
    const std::filesystem::path path = TempPath("miniengine_video_writer_test.avi");
    MjpegAviWriter writer;
    std::string error;
    Require(writer.Open(path, 4, 2, 25, error), "the file opens: " + error);
    // Chunk contents are not decoded here; one odd-sized to check the padding.
    const std::vector<uint8_t> first = {1, 2, 3};
    const std::vector<uint8_t> second = {4, 5, 6, 7};
    Require(writer.WriteFrame(first, error) && writer.WriteFrame(second, error), "frames write: " + error);
    const uint64_t projected = writer.GetProjectedFileBytes();
    Require(writer.Close(error), "the file closes: " + error);
    Require(std::filesystem::file_size(path) == projected, "the projected size is the closed file's");

    const ParsedAvi avi = ParseAvi(path);
    std::filesystem::remove(path);
    Require(avi.width == 4 && avi.height == 2, "the header has the frame size");
    Require(avi.microsecondsPerFrame == 40'000, "25 frames a second last 40 ms each");
    Require(avi.totalFrames == 2 && avi.frames[0] == first && avi.frames[1] == second, "the frames come back in order");
}

void TestEveryFramePacing()
{
    const std::filesystem::path path = TempPath("miniengine_video_every_frame_test.avi");
    VideoRecordingSettings settings;
    settings.path = path;
    settings.width = 32;
    settings.height = 16;
    settings.framesPerSecond = 30;
    settings.pacing = VideoPacing::EveryFrame;
    VideoRecorder recorder;
    std::string error;
    Require(recorder.Start(settings, error), "the recording starts: " + error);
    // Times far apart and all equal: neither matters with EveryFrame.
    for (int frame = 0; frame < 20; ++frame)
    {
        const uint8_t level = static_cast<uint8_t>(frame * 12);
        recorder.Submit(SplitFrame(32, 16, {level, 0, 0}, {0, 0, 255}, frame % 2 == 0 ? 0.0 : 100.0));
    }
    const VideoRecordingStatus status = recorder.Stop();
    Require(status.error.empty(), "the recording has no error: " + status.error);
    Require(status.framesWritten == 20 && status.framesDropped == 0, "every frame is one video frame");
    Require(status.files.size() == 1 && status.files[0] == path, "one file");
    Require(status.bytesWritten == std::filesystem::file_size(path), "the status has the file's size");

    const ParsedAvi avi = ParseAvi(path);
    std::filesystem::remove(path);
    Require(avi.totalFrames == 20, "the file has every frame");
    // The workers encode out of order; the file has them in the order they were submitted.
    for (int frame = 0; frame < 20; ++frame)
    {
        const std::array<int, 3> left = DecodePixel(avi.frames[frame], 4, 8, 32, 16);
        Require(Near(left, frame * 12, 0, 0), "frame " + std::to_string(frame) + " is in its place");
    }
    Require(Near(DecodePixel(avi.frames[0], 28, 8, 32, 16), 0, 0, 255), "the image is not mirrored or swizzled");
}

void TestRealTimePacing()
{
    const std::filesystem::path path = TempPath("miniengine_video_real_time_test.avi");
    VideoRecordingSettings settings;
    settings.path = path;
    settings.width = 16;
    settings.height = 16;
    settings.framesPerSecond = 10;
    settings.pacing = VideoPacing::RealTime;
    VideoRecorder recorder;
    std::string error;
    Require(recorder.Start(settings, error), "the recording starts: " + error);

    const double start = 50.0;
    Require(recorder.ClaimFrameAt(start), "the first frame is wanted");
    // The renderer claims a frame when it records the copy and submits it frames later.
    Require(!recorder.ClaimFrameAt(start + 0.02), "a claimed video frame is not wanted again before it arrives");
    recorder.Submit(SplitFrame(16, 16, {255, 0, 0}, {255, 0, 0}, start));
    Require(!recorder.ClaimFrameAt(start + 0.05), "a frame inside the first frame's time is not");
    recorder.Submit(SplitFrame(16, 16, {0, 0, 0}, {0, 0, 0}, start + 0.05)); // skipped
    Require(recorder.ClaimFrameAt(start + 0.1), "the next frame's time is");
    // Shown at 0.35 s: the red frame stays until then, video frames 0-2 (0 to 0.3 s); this one is
    // frame 3.
    recorder.Submit(SplitFrame(16, 16, {0, 255, 0}, {0, 255, 0}, start + 0.35));
    // Shown at 0.5 s, frame 5: the green one stays for frames 3 and 4.
    recorder.Submit(SplitFrame(16, 16, {0, 0, 255}, {0, 0, 255}, start + 0.5));
    const VideoRecordingStatus status = recorder.Stop();
    Require(status.error.empty(), "the recording has no error: " + status.error);
    Require(status.framesWritten == 6, "the video lasts as long as the frames took: " + std::to_string(status.framesWritten));
    Require(status.videoSeconds > 0.59 && status.videoSeconds < 0.61, "six frames at 10 fps last 0.6 s");

    const ParsedAvi avi = ParseAvi(path);
    std::filesystem::remove(path);
    Require(avi.totalFrames == 6, "the file has six frames");
    const int expected[6][3] = {{255, 0, 0}, {255, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 255, 0}, {0, 0, 255}};
    for (int frame = 0; frame < 6; ++frame)
    {
        Require(
            Near(DecodePixel(avi.frames[frame], 8, 8, 16, 16), expected[frame][0], expected[frame][1], expected[frame][2]),
            "frame " + std::to_string(frame) + " shows what was on screen then");
    }
}

void TestFullFileContinuesInTheNext()
{
    const std::filesystem::path path = TempPath("miniengine_video_split_test.avi");
    VideoRecordingSettings settings;
    settings.path = path;
    settings.width = 16;
    settings.height = 16;
    settings.pacing = VideoPacing::EveryFrame;
    // Room for the headers and a few small frames per file.
    settings.maxFileBytes = 2500;
    VideoRecorder recorder;
    std::string error;
    Require(recorder.Start(settings, error), "the recording starts: " + error);
    for (int frame = 0; frame < 12; ++frame)
    {
        recorder.Submit(SplitFrame(16, 16, {200, 100, 50}, {50, 100, 200}, frame));
    }
    const VideoRecordingStatus status = recorder.Stop();
    Require(status.error.empty(), "the recording has no error: " + status.error);
    Require(status.files.size() >= 2, "a full file goes on in the next");
    Require(status.files[1].filename() == "miniengine_video_split_test_2.avi", "the next file is numbered");
    uint32_t frames = 0;
    uint64_t bytes = 0;
    for (const std::filesystem::path& file : status.files)
    {
        Require(std::filesystem::file_size(file) <= settings.maxFileBytes, "no file outgrows the limit");
        bytes += std::filesystem::file_size(file);
        frames += ParseAvi(file).totalFrames;
        std::filesystem::remove(file);
    }
    Require(frames == 12, "the files hold every frame between them");
    Require(bytes == status.bytesWritten, "the status counts every file's bytes");
}
}

int main()
{
    try
    {
        TestFramesStartedBy();
        TestPixelConversion();
        TestWriterProducesAPlayableAvi();
        TestEveryFramePacing();
        TestRealTimePacing();
        TestFullFileContinuesInTheNext();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "video recorder tests passed\n";
    return 0;
}
