#pragma once

// Even frame starts for HDR output. Measured on an RTX 4070 laptop at 240 Hz (Windows 11, NVIDIA's
// Vulkan driver): while the GPU is saturated, an HDR10 swapchain hands its images back in batches,
// so presents came in a repeating 1 / 8 / 17 ms rhythm at the same 8 ms average (the scene judders),
// in every present mode, windowed or fullscreen, HDR10 or scRGB; SDR stayed even. With a little GPU
// idle time each frame the images come back at once: starting frames every 10 ms gave 10.0 ms
// +- 0.15 ms. The pacer keeps that idle time by starting frames at the GPU's average frame time
// times kHeadroom.

#include <chrono>
#include <optional>

namespace me
{

class FramePacer
{
  public:
    using Clock = std::chrono::steady_clock;

    // The interval over the GPU's average frame time.
    static constexpr double kHeadroom = 1.08;

    // When the frame about to start should start. now is the earliest it can; gpuFrameMs is the GPU's
    // recent average frame time (0 or less while unknown: no pacing). A frame that is already a whole
    // interval late starts now and the rhythm restarts from it.
    Clock::time_point Next(Clock::time_point now, double gpuFrameMs);
    void Reset();

  private:
    std::optional<Clock::time_point> m_next;
};

// Sleeps until time, the last stretch spinning so the wake-up is not a scheduler tick late.
void WaitUntil(FramePacer::Clock::time_point time);
}
