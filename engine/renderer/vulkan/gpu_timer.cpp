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

VulkanGpuTimer::VulkanGpuTimer(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t graphicsFamily, uint32_t frameCount)
    : m_device(device), m_slotMarks(frameCount), m_slotPending(frameCount, false)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    const uint32_t validBits = graphicsFamily < familyCount ? families[graphicsFamily].timestampValidBits : 0;
    m_supported = validBits > 0 && properties.limits.timestampPeriod > 0.0f;
    if (!m_supported)
    {
        return;
    }
    m_nanosecondsPerTick = properties.limits.timestampPeriod;
    m_validMask = validBits >= 64 ? ~0ull : (1ull << validBits) - 1;

    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = (kMaxMarks + 1) * frameCount;
    CheckVulkan(vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_pool), "Failed to create the GPU timer query pool");
}

VulkanGpuTimer::~VulkanGpuTimer()
{
    if (m_pool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(m_device, m_pool, nullptr);
    }
}

void VulkanGpuTimer::BeginFrame(VkCommandBuffer commandBuffer, uint32_t frameSlot)
{
    if (!m_supported)
    {
        return;
    }
    Collect(frameSlot);
    m_recordingSlot = frameSlot;
    m_slotMarks[frameSlot].clear();
    const uint32_t first = frameSlot * (kMaxMarks + 1);
    vkCmdResetQueryPool(commandBuffer, m_pool, first, kMaxMarks + 1);
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, first);
    m_slotPending[frameSlot] = true;
}

void VulkanGpuTimer::Mark(VkCommandBuffer commandBuffer, const char* name)
{
    std::vector<const char*>& marks = m_slotMarks[m_recordingSlot];
    if (!m_supported || marks.size() >= kMaxMarks)
    {
        return;
    }
    marks.push_back(name);
    const uint32_t query = m_recordingSlot * (kMaxMarks + 1) + static_cast<uint32_t>(marks.size());
    vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, query);
}

void VulkanGpuTimer::Collect(uint32_t frameSlot)
{
    const std::vector<const char*>& marks = m_slotMarks[frameSlot];
    if (!m_slotPending[frameSlot] || marks.empty())
    {
        return;
    }
    m_slotPending[frameSlot] = false;
    std::vector<uint64_t> ticks(marks.size() + 1);
    const VkResult result = vkGetQueryPoolResults(
        m_device,
        m_pool,
        frameSlot * (kMaxMarks + 1),
        static_cast<uint32_t>(ticks.size()),
        ticks.size() * sizeof(uint64_t),
        ticks.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        return;
    }
    const auto toMs = [this](uint64_t from, uint64_t to)
    {
        return static_cast<double>((to - from) & m_validMask) * m_nanosecondsPerTick * 1e-6;
    };

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
        PushSample(m_sections[index].samples, m_sampleCursor, toMs(ticks[index], ticks[index + 1]));
    }
    PushSample(m_frameSamples, m_sampleCursor, toMs(ticks.front(), ticks.back()));
    ++m_sampleCursor;
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
