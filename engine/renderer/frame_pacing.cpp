#include "frame_pacing.h"

#include <thread>

namespace me
{

FramePacer::Clock::time_point FramePacer::Next(Clock::time_point now, double gpuFrameMs)
{
    if (!(gpuFrameMs > 0.0))
    {
        Reset();
        return now;
    }
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(gpuFrameMs * kHeadroom));
    Clock::time_point next = m_next.has_value() ? *m_next + interval : now;
    if (next + interval < now)
    {
        next = now;
    }
    m_next = next;
    return next;
}

void FramePacer::Reset()
{
    m_next.reset();
}

void WaitUntil(FramePacer::Clock::time_point time)
{
    // Sleeps are 1 ms grained at best (SDL raises the Windows timer resolution); spin the rest.
    constexpr auto kSpin = std::chrono::microseconds(1500);
    while (true)
    {
        const auto left = time - FramePacer::Clock::now();
        if (left <= FramePacer::Clock::duration::zero())
        {
            return;
        }
        if (left > kSpin)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else
        {
            std::this_thread::yield();
        }
    }
}
}
