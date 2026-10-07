#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

namespace me
{

// What the frames in flight may still read goes here instead of being destroyed at once: each release
// runs once every frame submitted before it was retired has finished (VulkanCommandContext's submit
// count). A change of content no longer waits for the GPU to drain before it frees what it drops; on a
// streamed map that wait cost a frame or two at every change.
//
// Used from the render thread only (and under RunExclusive).
class VulkanRetireQueue
{
  public:
    // Runs release once the submits up to lastSubmit have finished.
    void Retire(uint64_t lastSubmit, std::function<void()> release)
    {
        m_entries.emplace_back(lastSubmit, std::move(release));
    }

    // Runs the releases whose submits have finished, oldest first.
    void Collect(uint64_t completedSubmits)
    {
        while (!m_entries.empty() && m_entries.front().first <= completedSubmits)
        {
            std::function<void()> release = std::move(m_entries.front().second);
            m_entries.pop_front();
            release();
        }
    }

    // Runs every release: the device is idle.
    void Flush()
    {
        Collect(UINT64_MAX);
    }

    bool Empty() const
    {
        return m_entries.empty();
    }

  private:
    std::deque<std::pair<uint64_t, std::function<void()>>> m_entries;
};
}
