#include "cpu_stage_timer.h"

#include <algorithm>

namespace me
{

void CpuStageTimer::BeginFrame()
{
    if (m_running)
    {
        Commit();
    }
    m_running = true;
    m_last = std::chrono::steady_clock::now();
}

void CpuStageTimer::Mark(const char* name)
{
    if (!m_running)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    Add(name, std::chrono::duration<double, std::milli>(now - m_last).count());
    m_last = now;
}

void CpuStageTimer::Add(const char* name, double milliseconds)
{
    Samples& stage = Find(name);
    stage.frameMs += milliseconds;
    stage.touched = true;
}

std::vector<CpuStageTimer::Stage> CpuStageTimer::GetStages() const
{
    std::vector<Stage> stages;
    stages.reserve(m_stages.size());
    for (const Samples& samples : m_stages)
    {
        double sum = 0.0;
        for (double ms : samples.ms)
        {
            sum += ms;
        }
        stages.push_back(Stage{samples.name, samples.ms.empty() ? 0.0 : sum / static_cast<double>(samples.ms.size())});
    }
    return stages;
}

std::vector<CpuStageTimer::Stage> CpuStageTimer::GetCurrentFrame() const
{
    std::vector<Stage> stages;
    for (const Samples& samples : m_stages)
    {
        if (samples.touched)
        {
            stages.push_back(Stage{samples.name, samples.frameMs});
        }
    }
    std::sort(stages.begin(), stages.end(), [](const Stage& a, const Stage& b)
              {
                  return a.averageMs > b.averageMs;
              });
    return stages;
}

CpuStageTimer::Samples& CpuStageTimer::Find(const char* name)
{
    for (Samples& samples : m_stages)
    {
        if (samples.name == name)
        {
            return samples;
        }
    }
    m_stages.push_back(Samples{name});
    return m_stages.back();
}

void CpuStageTimer::Commit()
{
    // A stage the frame skipped counts as zero, so the averages add up to the frame.
    for (Samples& samples : m_stages)
    {
        const double value = samples.touched ? samples.frameMs : 0.0;
        if (samples.ms.size() < kAverageFrames)
        {
            samples.ms.push_back(value);
        }
        else
        {
            samples.ms[samples.cursor % kAverageFrames] = value;
        }
        ++samples.cursor;
        samples.frameMs = 0.0;
        samples.touched = false;
    }
}
}
