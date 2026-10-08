#pragma once

// Even frame starts for HDR output. Measured on an RTX 4070 laptop at 240 Hz (Windows 11, NVIDIA's
// Vulkan driver, PresentMon): the driver presents an HDR swapchain through DXGI (SDR goes its own
// way), and while frames queue up it hands the images back in batches, so frames start in an uneven
// rhythm (three per ~10 ms in a light scene) and motion judders: with path tracing the step between
// what consecutive displayed frames show was off from the display's own step by 5.3 ms on average
// (SDR: 2.2 ms), windowed or fullscreen. Starting frames at the GPU's average frame time times
// kHeadroom keeps a little GPU idle time each frame, and the images come back as they are presented.
// The GPU's time must be its work alone: the frame's present pass waits for the swapchain image
// (VulkanCommandContext::RecordCommandBuffers), and counting that wait would pace by the batching.

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
