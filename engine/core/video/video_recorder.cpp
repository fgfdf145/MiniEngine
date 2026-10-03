#include "video_recorder.h"

// Its own copy of stb's writer, internal to this file: engine_core does not link engine_asset, which
// compiles the shared one.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
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
    m_writer.SetMaxFileBytes(m_settings.maxFileBytes);
    if (!m_writer.Open(SegmentPath(0), settings.width, settings.height, settings.framesPerSecond, error))
    {
        return false;
    }
    m_started = true;
    m_status = {};
    m_status.recording = true;
    m_status.files.push_back(SegmentPath(0));

    // JPEG encoding is what takes the time; leave a core for the render thread.
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
    const uint64_t lastFileBytes = m_writer.GetProjectedFileBytes();
    const bool closed = m_writer.Close(error);
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
        encoded.jpeg.reserve(rgba.size() / 8);
        stbi_write_jpg_to_func(
            AppendBytes,
            &encoded.jpeg,
            static_cast<int>(m_settings.width),
            static_cast<int>(m_settings.height),
            4,
            rgba.data(),
            m_settings.jpegQuality);

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
            if (!WriteJpeg(m_previousJpeg))
            {
                return;
            }
        }
    }
    if (WriteJpeg(encoded.jpeg))
    {
        m_previousJpeg = std::move(encoded.jpeg);
    }
}

bool VideoRecorder::WriteJpeg(const std::vector<uint8_t>& jpeg)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_status.error.empty())
        {
            return false;
        }
    }
    std::string error;
    if (!m_writer.CanFit(jpeg.size()))
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
    const bool written = m_writer.WriteFrame(jpeg, error);
    std::lock_guard lock(m_mutex);
    if (!written)
    {
        m_status.error = error;
        return false;
    }
    ++m_status.framesWritten;
    m_status.bytesWritten = m_finishedFileBytes + m_writer.GetProjectedFileBytes();
    m_status.videoSeconds = static_cast<double>(m_status.framesWritten) / m_settings.framesPerSecond;
    return true;
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
