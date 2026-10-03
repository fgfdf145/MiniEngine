#pragma once

#include "mjpeg_avi_writer.h"
#include "mp4_h264_writer.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace me
{

// How a frame's pixels are stored, top row first.
enum class VideoPixelFormat
{
    Rgba8,
    Bgra8,
    // Half-float RGBA, display-linear with 1.0 as white (the HDR output's LDR target): clipped
    // and sRGB-encoded on the way in, as the viewport capture's PNG is.
    RgbaHalf,
};

enum class VideoPacing
{
    // The video plays at the speed things happened: each frame lasts until the next one came, a
    // frame repeated or dropped to keep the constant frame rate. For recording the editor live.
    RealTime,
    // Every frame submitted is one video frame, however long it took to render. For scripted runs,
    // whose frames step a fixed time.
    EveryFrame,
};

// What a recording is written as, chosen by the file's extension (VideoCodecForPath).
enum class VideoCodec
{
    // .avi: every frame a JPEG. Any platform; large files, and some players decode its full-range
    // YUV as limited range, so the colours come out with too much contrast.
    MjpegAvi,
    // .mp4: H.264 through Media Foundation, BT.709 limited range. Windows only. For sharing.
    H264Mp4,
};

VideoCodec VideoCodecForPath(const std::filesystem::path& path);

struct VideoRecordingSettings
{
    // An .mp4 (H264Mp4) or an .avi (MjpegAvi). Past MjpegAviWriter::kMaxFileBytes an AVI recording
    // continues in <stem>_2.avi, _3 ... An MP4 has 64-bit sizes and stays one file.
    std::filesystem::path path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t framesPerSecond = 30;
    // stb's JPEG quality, 1 to 100 (MjpegAvi).
    int jpegQuality = 90;
    // The H.264 bit rate (H264Mp4); 0 for Mp4H264Writer::DefaultBitsPerSecond.
    uint32_t bitsPerSecond = 0;
    VideoPacing pacing = VideoPacing::RealTime;
    // Where an AVI file ends and the next begins; lowered by tests.
    uint64_t maxFileBytes = MjpegAviWriter::kMaxFileBytes;
};

struct VideoFrame
{
    std::vector<uint8_t> pixels;
    VideoPixelFormat format = VideoPixelFormat::Rgba8;
    // When it was shown, in seconds on any clock that only goes forward; RealTime pacing counts from
    // the first frame's time.
    double timeSeconds = 0.0;
};

struct VideoRecordingStatus
{
    bool recording = false;
    // Video frames written, repeats included: the video is this long.
    uint64_t framesWritten = 0;
    // Frames handed over while the encoders were behind, left out (RealTime only).
    uint64_t framesDropped = 0;
    double videoSeconds = 0.0;
    // The files' size so far, index included.
    uint64_t bytesWritten = 0;
    // Why the recording stopped writing, if it did. Frames handed over afterwards are discarded.
    std::string error;
    std::vector<std::filesystem::path> files;
};

// How many frames of a constant-rate video have started by `seconds` after its first frame: the
// first frame starts at 0, so at least 1.
uint64_t VideoFramesStartedBy(double seconds, uint32_t framesPerSecond);

// Converts a frame to tightly packed RGBA8.
std::vector<uint8_t> ConvertVideoFrameToRgba8(const VideoFrame& frame, uint32_t width, uint32_t height);

// Converts tightly packed RGBA8 (sRGB-encoded) of width x height to NV12 in BT.709 limited range,
// keeping the top-left even width x height (an odd last column or row is cut: 4:2:0 needs pairs).
std::vector<uint8_t> ConvertRgba8ToNv12(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

// Records frames to an H.264 MP4 or an MJPEG AVI. Frames are converted (and JPEG-encoded) on worker
// threads and written in the order they were submitted, so Submit costs the caller a move. Not reusable after
// Stop: make a new one per recording.
class VideoRecorder
{
  public:
    VideoRecorder() = default;
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;
    ~VideoRecorder();

    // Opens the file and starts the workers. Returns false and fills error when the file cannot be
    // created or the settings are invalid.
    bool Start(const VideoRecordingSettings& settings, std::string& error);
    // Whether a frame shown at this time would make it into the video, for the caller to skip
    // reading back the ones that would not. With RealTime pacing, true once per video frame: the
    // first frame shown in its time claims it, though it is submitted frames later.
    bool ClaimFrameAt(double timeSeconds);
    // Hands a frame of the size Start was given to the workers. With RealTime pacing a frame is
    // dropped while the workers are behind; with EveryFrame the call waits for room instead.
    void Submit(VideoFrame frame);
    // Encodes and writes what was submitted, closes the file and returns what was written.
    VideoRecordingStatus Stop();
    VideoRecordingStatus GetStatus() const;
    const VideoRecordingSettings& GetSettings() const
    {
        return m_settings;
    }

  private:
    struct Job
    {
        uint64_t sequence = 0;
        VideoFrame frame;
    };
    struct Encoded
    {
        // A JPEG (MjpegAvi) or an NV12 frame for the H.264 encoder (H264Mp4).
        std::vector<uint8_t> data;
        double timeSeconds = 0.0;
    };

    void WorkerLoop();
    // Writes the encoded frames that are next in order; one thread at a time.
    void WriteReadyFrames();
    // Writes one encoded frame, after repeating the one before for as long as it was on screen
    // (RealTime). Called with m_writeMutex held.
    void WriteEncoded(Encoded&& encoded);
    bool WriteFrameData(const std::vector<uint8_t>& data);
    uint64_t CurrentFileBytes() const;
    std::filesystem::path SegmentPath(size_t segment) const;

    VideoRecordingSettings m_settings;
    VideoCodec m_codec = VideoCodec::MjpegAvi;
    size_t m_maxPending = 0;

    mutable std::mutex m_mutex;
    std::condition_variable m_jobReady;
    std::condition_variable m_roomReady;
    std::deque<Job> m_jobs;
    std::map<uint64_t, Encoded> m_encoded;
    // Submitted and not yet written.
    size_t m_pending = 0;
    uint64_t m_nextSequence = 0;
    uint64_t m_nextWriteSequence = 0;
    // RealTime pacing at submission: the first frame's time, the last video frame claimed and the
    // last handed over.
    bool m_haveSubmitTime = false;
    double m_submitFirstTime = 0.0;
    uint64_t m_lastClaimedSlot = 0;
    uint64_t m_lastSubmittedSlot = 0;
    bool m_stopping = false;
    bool m_started = false;
    VideoRecordingStatus m_status;

    // The writer's state: held by whichever worker writes.
    std::mutex m_writeMutex;
    MjpegAviWriter m_writer;
    Mp4H264Writer m_mp4Writer;
    // The bytes of the files already finished.
    uint64_t m_finishedFileBytes = 0;
    std::vector<uint8_t> m_previousFrame;
    bool m_haveFirstTime = false;
    double m_firstTime = 0.0;

    std::vector<std::thread> m_workers;
};
}
