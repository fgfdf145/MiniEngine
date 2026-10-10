#include "gpu_timer.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace me
{

namespace
{
double Average(const std::vector<double>& samples)
{
    return samples.empty() ? 0.0 : std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

void PushSample(std::vector<double>& samples, uint32_t cursor, double value)
{
    if (samples.size() < VulkanGpuTimer::kAverageFrames)
    {
        samples.push_back(value);
    }
    else
    {
        samples[cursor % VulkanGpuTimer::kAverageFrames] = value;
    }
}
}

VulkanGpuTimer::VulkanGpuTimer(nvrhi::IDevice* device, uint32_t frameCount)
    : m_device(device), m_queries(frameCount), m_slotMarks(frameCount), m_slotPending(frameCount, false)
{
    for (std::vector<nvrhi::TimerQueryHandle>& queries : m_queries)
    {
        for (uint32_t index = 0; index <= kMaxMarks; ++index)
        {
            queries.push_back(m_device->createTimerQuery());
        }
    }
}

VulkanGpuTimer::~VulkanGpuTimer() = default;

void VulkanGpuTimer::BeginFrame(nvrhi::ICommandList* commandList, uint32_t frameSlot)
{
    Collect(frameSlot);
    m_commandList = commandList;
    m_recordingSlot = frameSlot;
    m_slotMarks[frameSlot].clear();
    m_commandList->beginTimerQuery(m_queries[frameSlot][0]);
    m_slotPending[frameSlot] = true;
}

void VulkanGpuTimer::Mark(const char* name)
{
    std::vector<const char*>& marks = m_slotMarks[m_recordingSlot];
    if (m_commandList == nullptr || marks.size() >= kMaxMarks)
    {
        return;
    }
    if (m_debugMarkers)
    {
        m_commandList->beginMarker((std::string("end: ") + name).c_str());
        m_commandList->endMarker();
    }
    const std::vector<nvrhi::TimerQueryHandle>& queries = m_queries[m_recordingSlot];
    m_commandList->endTimerQuery(queries[marks.size()]);
    marks.push_back(name);
    m_commandList->beginTimerQuery(queries[marks.size()]);
}

void VulkanGpuTimer::Collect(uint32_t frameSlot)
{
    const std::vector<const char*>& marks = m_slotMarks[frameSlot];
    if (!m_slotPending[frameSlot] || marks.empty())
    {
        return;
    }
    m_slotPending[frameSlot] = false;
    // Each section's query, read in seconds; the frame is their sum (the gaps between one's end
    // and the next one's begin are two timestamps apart).
    std::vector<double> sectionMs(marks.size());
    for (size_t index = 0; index < marks.size(); ++index)
    {
        sectionMs[index] = static_cast<double>(m_device->getTimerQueryTime(m_queries[frameSlot][index])) * 1e3;
        m_device->resetTimerQuery(m_queries[frameSlot][index]);
    }

    // A different set of marks (a pass switched off, the order changed) starts the averages over.
    bool sameLayout = m_sections.size() == marks.size();
    for (size_t index = 0; sameLayout && index < marks.size(); ++index)
    {
        sameLayout = m_sections[index].name == marks[index];
    }
    if (!sameLayout)
    {
        m_sections.clear();
        for (const char* name : marks)
        {
            m_sections.push_back(Accumulator{name, {}});
        }
        m_frameSamples.clear();
        m_sampleCursor = 0;
    }
    for (size_t index = 0; index < marks.size(); ++index)
    {
        PushSample(m_sections[index].samples, m_sampleCursor, sectionMs[index]);
    }
    m_lastFrameMs = std::accumulate(sectionMs.begin(), sectionMs.end(), 0.0);
    PushSample(m_frameSamples, m_sampleCursor, m_lastFrameMs);
    ++m_sampleCursor;
}

double VulkanGpuTimer::GetLastFrameMs() const
{
    return m_lastFrameMs;
}

std::vector<VulkanGpuTimer::Section> VulkanGpuTimer::GetSections() const
{
    std::vector<Section> sections;
    sections.reserve(m_sections.size());
    for (const Accumulator& accumulator : m_sections)
    {
        sections.push_back(Section{accumulator.name, Average(accumulator.samples)});
    }
    return sections;
}

double VulkanGpuTimer::GetAverageFrameMs() const
{
    return Average(m_frameSamples);
}
}
