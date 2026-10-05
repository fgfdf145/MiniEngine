#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace me
{

// Where a frame's CPU time goes: BeginFrame, then Mark(name) after each stage, which charges the time
// since the previous mark to that name. Each stage keeps the average of its last kAverageFrames frames,
// as VulkanGpuTimer does for the GPU, and the frame-timing log prints them side by side.
class CpuStageTimer
{
  public:
    static constexpr size_t kAverageFrames = 120;

    struct Stage
    {
        std::string name;
        double averageMs = 0.0;
    };

    void BeginFrame();
    void Mark(const char* name);
    // Charges a measured duration to a stage without moving the running mark (time spent in a
    // callback the frame does not own, for example).
    void Add(const char* name, double milliseconds);

    // In the order the stages were first marked.
    std::vector<Stage> GetStages() const;
    // This frame's stages so far (averageMs holds this frame's time), the slowest first: what a slow
    // frame is logged with.
    std::vector<Stage> GetCurrentFrame() const;

  private:
    struct Samples
    {
        std::string name;
        std::vector<double> ms;
        size_t cursor = 0;
        double frameMs = 0.0; // this frame's total so far
        bool touched = false;
    };
    Samples& Find(const char* name);
    void Commit();

    std::vector<Samples> m_stages;
    std::chrono::steady_clock::time_point m_last{};
    bool m_running = false;
};
}
