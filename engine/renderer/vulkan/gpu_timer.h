#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

// GPU time per section of the frame's command list, from NVRHI timer queries. Each frame slot owns
// kMaxMarks queries; BeginFrame reads what the slot recorded last time (its frame has completed by
// then, so the results are ready) and starts a new frame on the frame's command list, and each Mark
// closes the section since the previous mark. Sections are averaged over the last kAverageFrames
// frames.
class VulkanGpuTimer
{
  public:
    static constexpr uint32_t kMaxMarks = 63;
    static constexpr uint32_t kAverageFrames = 120;

    struct Section
    {
        std::string name;
        double averageMs = 0.0;
    };

    VulkanGpuTimer(nvrhi::IDevice* device, uint32_t frameCount);
    ~VulkanGpuTimer();

    VulkanGpuTimer(const VulkanGpuTimer&) = delete;
    VulkanGpuTimer& operator=(const VulkanGpuTimer&) = delete;

    // Marks go to commandList, the frame's, until the next BeginFrame.
    void BeginFrame(nvrhi::ICommandList* commandList, uint32_t frameSlot);
    // Ends the section that started at the previous mark (or BeginFrame).
    void Mark(const char* name);
    // Each Mark also leaves a debug marker named "end: <name>" in the command list, so a frame capture
    // (RenderDoc, --renderdoc) shows where each timed pass ends. Off by default.
    void SetDebugMarkers(bool enabled)
    {
        m_debugMarkers = enabled;
    }

    // In recording order; empty until a frame has come back.
    std::vector<Section> GetSections() const;
    double GetAverageFrameMs() const;
    // The whole of the last frame that came back (0 before any).
    double GetLastFrameMs() const;

  private:
    void Collect(uint32_t frameSlot);

    struct Accumulator
    {
        std::string name;
        std::vector<double> samples;
    };

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::ICommandList* m_commandList = nullptr;
    // Per slot: kMaxMarks + 1 queries, one a section; the one after the last mark is begun and never
    // ended.
    std::vector<std::vector<nvrhi::TimerQueryHandle>> m_queries;
    // What each slot recorded: its mark names, in order, and whether its queries are in flight.
    std::vector<std::vector<const char*>> m_slotMarks;
    std::vector<bool> m_slotPending;
    uint32_t m_recordingSlot = 0;
    std::vector<Accumulator> m_sections;
    std::vector<double> m_frameSamples;
    uint32_t m_sampleCursor = 0;
    double m_lastFrameMs = 0.0;
    bool m_debugMarkers = false;
};
}
