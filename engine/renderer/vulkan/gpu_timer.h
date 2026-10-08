#pragma once

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

// GPU time per section of the frame's command buffer, from timestamp queries. Each frame slot
// owns a range of one query pool; BeginFrame reads what the slot recorded last time (its fence has
// signaled by then, so the results are ready) and starts a new frame, and each Mark closes the
// section since the previous mark. Sections are averaged over the last kAverageFrames frames.
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

    VulkanGpuTimer(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t graphicsFamily, uint32_t frameCount);
    ~VulkanGpuTimer();

    VulkanGpuTimer(const VulkanGpuTimer&) = delete;
    VulkanGpuTimer& operator=(const VulkanGpuTimer&) = delete;

    void BeginFrame(VkCommandBuffer commandBuffer, uint32_t frameSlot);
    // Ends the section that started at the previous mark (or BeginFrame).
    void Mark(VkCommandBuffer commandBuffer, const char* name);

    // In recording order; empty until a frame has come back.
    std::vector<Section> GetSections() const;
    double GetAverageFrameMs() const;
    // The frame's average up to the section called name (the whole frame when there is none): the
    // GPU's own work, without a last section that waits for the presentation engine.
    double GetAverageFrameMsBefore(const char* name) const;

  private:
    void Collect(uint32_t frameSlot);

    struct Accumulator
    {
        std::string name;
        std::vector<double> samples;
    };

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    bool m_supported = false;
    double m_nanosecondsPerTick = 1.0;
    uint64_t m_validMask = ~0ull;
    // What each slot recorded: its mark names, in order, and whether its queries are in flight.
    std::vector<std::vector<const char*>> m_slotMarks;
    std::vector<bool> m_slotPending;
    uint32_t m_recordingSlot = 0;
    std::vector<Accumulator> m_sections;
    std::vector<double> m_frameSamples;
    uint32_t m_sampleCursor = 0;
};
}
