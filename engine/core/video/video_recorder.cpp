#include "video_recorder.h"

// Its own copy of stb's writer, internal to this file: engine_core does not link engine_asset, which
// compiles the shared one.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace me
{

namespace
{
float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    const uint32_t mantissa = half & 0x3FFu;
    float magnitude = 0.0f;
    if (exponent == 0)
    {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    }
    else if (exponent == 31)
    {
        magnitude = mantissa == 0 ? INFINITY : NAN;
    }
    else
    {
        magnitude = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -magnitude : magnitude;
}

uint8_t EncodeSrgb(float linear)
{
    if (!(linear > 0.0f))
    {
        return 0;
    }
    const float clamped = std::min(linear, 1.0f);
    const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}

void AppendBytes(void* context, void* data, int size)
{
    auto* output = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    output->insert(output->end(), bytes, bytes + size);
}
}

VideoCodec VideoCodecForPath(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".mp4" ? VideoCodec::H264Mp4 : VideoCodec::MjpegAvi;
}

uint64_t VideoFramesStartedBy(double seconds, uint32_t framesPerSecond)
{
    if (!(seconds > 0.0))
    {
        return 1;
    }
    // A frame shown exactly on a frame boundary (n / fps, rounded) starts that frame.
    return static_cast<uint64_t>(std::floor(seconds * framesPerSecond + 1e-6)) + 1;
}

std::vector<uint8_t> ConvertVideoFrameToRgba8(const VideoFrame& frame, uint32_t width, uint32_t height)
{
    const size_t texels = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(texels * 4);
    switch (frame.format)
    {
    case VideoPixelFormat::Rgba8:
    case VideoPixelFormat::Bgra8:
        std::memcpy(rgba.data(), frame.pixels.data(), std::min(rgba.size(), frame.pixels.size()));
        if (frame.format == VideoPixelFormat::Bgra8)
        {
            for (size_t texel = 0; texel < texels; ++texel)
            {
                std::swap(rgba[texel * 4], rgba[texel * 4 + 2]);
            }
        }
        break;
    case VideoPixelFormat::RgbaHalf:
        for (size_t channel = 0; channel < std::min(rgba.size(), frame.pixels.size() / 2); ++channel)
        {
            uint16_t half = 0;
            std::memcpy(&half, frame.pixels.data() + channel * 2, sizeof(half));
            rgba[channel] = EncodeSrgb(HalfToFloat(half));
        }
        break;
    }
    for (size_t texel = 0; texel < texels; ++texel)
    {
        rgba[texel * 4 + 3] = 255;
    }
    return rgba;
}

std::vector<uint8_t> ConvertRgba8ToNv12(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)
{
    // BT.709 in 16.16 fixed point, scaled to limited range: Y 16..235, Cb Cr 16..240 about 128.
    // Y = 16 + 219 (0.2126 R + 0.7152 G + 0.0722 B), Cb = 128 + 224 (B - Y) / 1.8556,
    // Cr = 128 + 224 (R - Y) / 1.5748, with R G B and Y from 0 to 1.
    constexpr int32_t kYr = 11966, kYg = 40254, kYb = 4064;
    constexpr int32_t kCbR = -6596, kCbG = -22189, kCbB = 28784;
    constexpr int32_t kCrR = 28784, kCrG = -26145, kCrB = -2639;
    const uint32_t outWidth = width & ~1u;
    const uint32_t outHeight = height & ~1u;
    std::vector<uint8_t> nv12(static_cast<size_t>(outWidth) * outHeight * 3 / 2);
    if (rgba.size() < static_cast<size_t>(width) * height * 4)
    {
        return nv12;
    }
    uint8_t* luma = nv12.data();
    uint8_t* chroma = nv12.data() + static_cast<size_t>(outWidth) * outHeight;
    for (uint32_t y = 0; y < outHeight; y += 2)
    {
        const uint8_t* rows[2] = {
            rgba.data() + static_cast<size_t>(y) * width * 4,
            rgba.data() + static_cast<size_t>(y + 1) * width * 4};
        uint8_t* lumaRows[2] = {luma + static_cast<size_t>(y) * outWidth, luma + static_cast<size_t>(y + 1) * outWidth};
        uint8_t* chromaRow = chroma + static_cast<size_t>(y / 2) * outWidth;
        for (uint32_t x = 0; x < outWidth; x += 2)
        {
            int32_t r = 0, g = 0, b = 0;
            for (int row = 0; row < 2; ++row)
            {
                for (uint32_t column = x; column < x + 2; ++column)
                {
                    const uint8_t* texel = rows[row] + column * 4;
                    lumaRows[row][column] = static_cast<uint8_t>(
                        16 + ((kYr * texel[0] + kYg * texel[1] + kYb * texel[2] + 32768) >> 16));
                    r += texel[0];
                    g += texel[1];
                    b += texel[2];
                }
            }
            // The four texels' average, its quarter folded into the shift.
            const int32_t cb = 128 + ((kCbR * r + kCbG * g + kCbB * b + (1 << 17)) >> 18);
            const int32_t cr = 128 + ((kCrR * r + kCrG * g + kCrB * b + (1 << 17)) >> 18);
            chromaRow[x] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
            chromaRow[x + 1] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
        }
    }
    return nv12;
}

VideoRecorder::~VideoRecorder()
{
    Stop();
}

bool VideoRecorder::Start(const VideoRecordingSettings& settings, std::string& error)
{
    if (m_started)
    {
        error = "The recorder has already been used";
        return false;
    }
    m_settings = settings;
    m_settings.jpegQuality = std::clamp(m_settings.jpegQuality, 1, 100);
    m_codec = VideoCodecForPath(settings.path);
    if (m_codec == VideoCodec::H264Mp4)
    {
        // 4:2:0 needs an even size: an odd last column or row is left out.
        const uint32_t width = settings.width & ~1u;
        const uint32_t height = settings.height & ~1u;
        if (m_settings.bitsPerSecond == 0)
        {
            m_settings.bitsPerSecond = Mp4H264Writer::DefaultBitsPerSecond(width, height, settings.framesPerSecond);
        }
        if (!m_mp4Writer.Open(settings.path, width, height, settings.framesPerSecond, m_settings.bitsPerSecond, error))
        {
            return false;
        }
    }
    else
    {
        m_writer.SetMaxFileBytes(m_settings.maxFileBytes);
        if (!m_writer.Open(SegmentPath(0), settings.width, settings.height, settings.framesPerSecond, error))
        {
            return false;
        }
    }
    m_started = true;
    m_status = {};
    m_status.recording = true;
    m_status.files.push_back(SegmentPath(0));

    // JPEG encoding (or the NV12 conversion) is what takes the time; leave a core for the render thread.
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const unsigned workers = std::clamp(cores - 1, 1u, 4u);
    // Each pending frame holds its pixels: a few per worker absorbs a hitch without much memory.
    m_maxPending = workers * 2;
    for (unsigned worker = 0; worker < workers; ++worker)
    {
        m_workers.emplace_back(&VideoRecorder::WorkerLoop, this);
    }
    return true;
}

bool VideoRecorder::ClaimFrameAt(double timeSeconds)
{
    std::lock_guard lock(m_mutex);
    if (!m_status.recording || m_stopping || !m_status.error.empty())
    {
        return false;
    }
    if (m_settings.pacing != VideoPacing::RealTime)
    {
        return true;
    }
    if (!m_haveSubmitTime)
    {
        m_haveSubmitTime = true;
        m_submitFirstTime = timeSeconds;
    }
    const uint64_t slot = VideoFramesStartedBy(timeSeconds - m_submitFirstTime, m_settings.framesPerSecond);
    if (slot <= m_lastClaimedSlot)
    {
        return false;
    }
    m_lastClaimedSlot = slot;
    return true;
}

void VideoRecorder::Submit(VideoFrame frame)
{
    std::unique_lock lock(m_mutex);
    if (!m_status.recording || m_stopping || !m_status.error.empty())
    {
        return;
    }
    if (m_settings.pacing == VideoPacing::RealTime)
    {
        if (!m_haveSubmitTime)
        {
            m_haveSubmitTime = true;
            m_submitFirstTime = frame.timeSeconds;
        }
        // A frame for a video frame that already has one would not be written: skip its encoding.
        const uint64_t slot = VideoFramesStartedBy(frame.timeSeconds - m_submitFirstTime, m_settings.framesPerSecond);
        if (slot <= m_lastSubmittedSlot)
        {
            return;
        }
        m_lastSubmittedSlot = slot;
    }
    if (m_pending >= m_maxPending)
    {
        if (m_settings.pacing == VideoPacing::RealTime)
        {
            // The next frame written is repeated to cover this one's time.
            ++m_status.framesDropped;
            return;
        }
        m_roomReady.wait(lock, [this]
                         {
                             return m_pending < m_maxPending || m_stopping || !m_status.recording;
                         });
        if (m_stopping || !m_status.recording)
        {
            return;
        }
    }
    ++m_pending;
    m_jobs.push_back(Job{m_nextSequence++, std::move(frame)});
    m_jobReady.notify_one();
}

VideoRecordingStatus VideoRecorder::Stop()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || m_stopping)
        {
            return m_status;
        }
        m_stopping = true;
    }
    m_jobReady.notify_all();
    m_roomReady.notify_all();
    for (std::thread& worker : m_workers)
    {
        worker.join();
    }
    m_workers.clear();
    // A worker that encoded the last frames may have left them to a writer that had just finished.
    WriteReadyFrames();

    std::lock_guard writeLock(m_writeMutex);
    std::string error;
    uint64_t lastFileBytes = 0;
    bool closed = false;
    if (m_codec == VideoCodec::H264Mp4)
    {
        // The encoder's last frames are written by Close: the file's size is known after it.
        closed = m_mp4Writer.Close(error);
        lastFileBytes = m_mp4Writer.GetFileBytes();
    }
    else
    {
        lastFileBytes = m_writer.GetProjectedFileBytes();
        closed = m_writer.Close(error);
    }
    std::lock_guard lock(m_mutex);
    m_status.bytesWritten = m_finishedFileBytes + lastFileBytes;
    if (!closed && m_status.error.empty())
    {
        m_status.error = error;
    }
    m_status.recording = false;
    return m_status;
}

VideoRecordingStatus VideoRecorder::GetStatus() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

void VideoRecorder::WorkerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_jobReady.wait(lock, [this]
                            {
                                return !m_jobs.empty() || m_stopping;
                            });
            if (m_jobs.empty())
            {
                return;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        Encoded encoded;
        encoded.timeSeconds = job.frame.timeSeconds;
        const std::vector<uint8_t> rgba = ConvertVideoFrameToRgba8(job.frame, m_settings.width, m_settings.height);
        job.frame.pixels = {};
        if (m_codec == VideoCodec::H264Mp4)
        {
            encoded.data = ConvertRgba8ToNv12(rgba, m_settings.width, m_settings.height);
        }
        else
        {
            encoded.data.reserve(rgba.size() / 8);
            stbi_write_jpg_to_func(
                AppendBytes,
                &encoded.data,
                static_cast<int>(m_settings.width),
                static_cast<int>(m_settings.height),
                4,
                rgba.data(),
                m_settings.jpegQuality);
        }

        {
            std::lock_guard lock(m_mutex);
            m_encoded.emplace(job.sequence, std::move(encoded));
        }
        WriteReadyFrames();
    }
}

void VideoRecorder::WriteReadyFrames()
{
    while (true)
    {
        std::unique_lock writeLock(m_writeMutex, std::try_to_lock);
        if (!writeLock.owns_lock())
        {
            // The thread that holds it looks again before it lets go.
            return;
        }
        while (true)
        {
            Encoded next;
            {
                std::lock_guard lock(m_mutex);
                const auto found = m_encoded.find(m_nextWriteSequence);
                if (found == m_encoded.end())
                {
                    break;
                }
                next = std::move(found->second);
                m_encoded.erase(found);
                ++m_nextWriteSequence;
            }
            WriteEncoded(std::move(next));
            {
                std::lock_guard lock(m_mutex);
                --m_pending;
            }
            m_roomReady.notify_all();
        }
        writeLock.unlock();
        // A frame that finished encoding while this thread was letting go found the lock held.
        std::lock_guard lock(m_mutex);
        if (m_encoded.find(m_nextWriteSequence) == m_encoded.end())
        {
            return;
        }
    }
}

void VideoRecorder::WriteEncoded(Encoded&& encoded)
{
    if (m_settings.pacing == VideoPacing::RealTime)
    {
        if (!m_haveFirstTime)
        {
            m_haveFirstTime = true;
            m_firstTime = encoded.timeSeconds;
        }
        uint64_t written = 0;
        {
            std::lock_guard lock(m_mutex);
            written = m_status.framesWritten;
        }
        const uint64_t due = VideoFramesStartedBy(encoded.timeSeconds - m_firstTime, m_settings.framesPerSecond);
        if (due <= written)
        {
            // Its video frame already shows the one before.
            return;
        }
        // What was on screen until this frame came stays there until its video frame.
        for (uint64_t repeat = written + 1; repeat < due; ++repeat)
        {
            if (!WriteFrameData(m_previousFrame))
            {
                return;
            }
        }
    }
    if (WriteFrameData(encoded.data))
    {
        m_previousFrame = std::move(encoded.data);
    }
}

bool VideoRecorder::WriteFrameData(const std::vector<uint8_t>& data)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_status.error.empty())
        {
            return false;
        }
    }
    std::string error;
    if (m_codec == VideoCodec::MjpegAvi && !m_writer.CanFit(data.size()))
    {
        // The file is as large as an AVI's 32-bit sizes allow: go on in the next one.
        const uint64_t fileBytes = m_writer.GetProjectedFileBytes();
        const size_t segment = [this]
        {
            std::lock_guard lock(m_mutex);
            return m_status.files.size();
        }();
        const bool reopened = m_writer.Close(error) &&
                              m_writer.Open(SegmentPath(segment), m_settings.width, m_settings.height, m_settings.framesPerSecond, error);
        std::lock_guard lock(m_mutex);
        m_finishedFileBytes += fileBytes;
        if (!reopened)
        {
            m_status.error = error;
            return false;
        }
        m_status.files.push_back(SegmentPath(segment));
    }
    const bool written = m_codec == VideoCodec::H264Mp4 ? m_mp4Writer.WriteFrame(data, error) : m_writer.WriteFrame(data, error);
    std::lock_guard lock(m_mutex);
    if (!written)
    {
        m_status.error = error;
        return false;
    }
    ++m_status.framesWritten;
    m_status.bytesWritten = m_finishedFileBytes + CurrentFileBytes();
    m_status.videoSeconds = static_cast<double>(m_status.framesWritten) / m_settings.framesPerSecond;
    return true;
}

uint64_t VideoRecorder::CurrentFileBytes() const
{
    return m_codec == VideoCodec::H264Mp4 ? m_mp4Writer.GetFileBytes() : m_writer.GetProjectedFileBytes();
}

std::filesystem::path VideoRecorder::SegmentPath(size_t segment) const
{
    if (segment == 0)
    {
        return m_settings.path;
    }
    std::filesystem::path path = m_settings.path;
    path.replace_filename(
        path.stem().string() + "_" + std::to_string(segment + 1) + path.extension().string());
    return path;
}
}
