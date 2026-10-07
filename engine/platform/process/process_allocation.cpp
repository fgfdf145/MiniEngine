#include "process_allocation.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sched.h>
#endif

namespace me
{

namespace platform::process
{
namespace
{
constexpr std::string_view kPriorityKeys[] = {"below_normal", "normal", "above_normal", "high"};
constexpr std::string_view kPriorityLabels[] = {"Below Normal", "Normal", "Above Normal", "High"};
constexpr std::string_view kSelectionKeys[] = {"all", "performance", "custom"};

ProcessorTopology UniformTopology(uint32_t count)
{
    ProcessorTopology topology;
    for (uint32_t index = 0; index < std::max(count, 1u); ++index)
    {
        topology.processors.push_back({index, index, 0});
    }
    return topology;
}

#if defined(_WIN32)
std::string WindowsErrorText(DWORD error)
{
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);
    std::string text = length > 0 ? std::string(buffer, length) : "error " + std::to_string(error);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.'))
    {
        text.pop_back();
    }
    return text;
}

DWORD PriorityClass(ProcessPriority priority)
{
    switch (priority)
    {
    case ProcessPriority::BelowNormal:
        return BELOW_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::Normal:
        return NORMAL_PRIORITY_CLASS;
    case ProcessPriority::AboveNormal:
        return ABOVE_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::High:
        return HIGH_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;
}
#else
// Nice values; below zero needs CAP_SYS_NICE (or root) on Linux.
int NiceValue(ProcessPriority priority)
{
    switch (priority)
    {
    case ProcessPriority::BelowNormal:
        return 5;
    case ProcessPriority::Normal:
        return 0;
    case ProcessPriority::AboveNormal:
        return -5;
    case ProcessPriority::High:
        return -10;
    }
    return 0;
}
#endif

void AppendError(std::string& errors, const std::string& error)
{
    errors += errors.empty() ? error : "; " + error;
}
}

bool ProcessorTopology::IsHybrid() const
{
    return std::any_of(processors.begin(), processors.end(), [&](const LogicalProcessor& processor)
                       {
                           return processor.efficiencyClass != processors.front().efficiencyClass;
                       });
}

bool ProcessorTopology::IsPerformance(const LogicalProcessor& processor) const
{
    uint8_t fastest = 0;
    for (const LogicalProcessor& other : processors)
    {
        fastest = std::max(fastest, other.efficiencyClass);
    }
    return processor.efficiencyClass == fastest;
}

uint32_t ProcessorTopology::PerformanceCount() const
{
    return static_cast<uint32_t>(std::count_if(processors.begin(), processors.end(), [&](const LogicalProcessor& processor)
                                               {
                                                   return IsPerformance(processor);
                                               }));
}

ProcessorTopology QueryProcessorTopology()
{
#if defined(_WIN32)
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0)
    {
        return UniformTopology(std::thread::hardware_concurrency());
    }
    const std::unique_ptr<std::byte[]> buffer(new std::byte[size]);
    auto* const first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &size))
    {
        return UniformTopology(std::thread::hardware_concurrency());
    }

    ProcessorTopology topology;
    uint32_t core = 0;
    for (DWORD offset = 0; offset < size;)
    {
        const auto* const entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get() + offset);
        offset += entry->Size;
        if (entry->Relationship != RelationProcessorCore)
        {
            continue;
        }
        const PROCESSOR_RELATIONSHIP& processor = entry->Processor;
        for (WORD group = 0; group < processor.GroupCount; ++group)
        {
            if (processor.GroupMask[group].Group != 0)
            {
                continue;
            }
            const KAFFINITY mask = processor.GroupMask[group].Mask;
            for (uint32_t bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit)
            {
                if ((mask >> bit) & 1)
                {
                    topology.processors.push_back({bit, core, processor.EfficiencyClass});
                }
            }
        }
        ++core;
    }
    if (topology.processors.empty())
    {
        return UniformTopology(std::thread::hardware_concurrency());
    }
    std::sort(topology.processors.begin(), topology.processors.end(), [](const LogicalProcessor& a, const LogicalProcessor& b)
              {
                  return a.index < b.index;
              });
    return topology;
#else
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return UniformTopology(online > 0 ? static_cast<uint32_t>(online) : std::thread::hardware_concurrency());
#endif
}

std::vector<uint32_t> ResolveCpuSelection(const ProcessAllocation& allocation, const ProcessorTopology& topology)
{
    std::vector<uint32_t> cpus;
    for (const LogicalProcessor& processor : topology.processors)
    {
        const bool chosen = allocation.cpus == CpuSelection::All ||
                            (allocation.cpus == CpuSelection::Performance && topology.IsPerformance(processor)) ||
                            (allocation.cpus == CpuSelection::Custom &&
                             std::find(allocation.customCpus.begin(), allocation.customCpus.end(), processor.index) !=
                                 allocation.customCpus.end());
        if (chosen)
        {
            cpus.push_back(processor.index);
        }
    }
    if (cpus.empty())
    {
        for (const LogicalProcessor& processor : topology.processors)
        {
            cpus.push_back(processor.index);
        }
    }
    return cpus;
}

ProcessAllocationResult ApplyProcessAllocation(const ProcessAllocation& allocation, const ProcessorTopology& topology)
{
    ProcessAllocationResult result;
    result.cpus = ResolveCpuSelection(allocation, topology);
#if defined(_WIN32)
    if (!SetPriorityClass(GetCurrentProcess(), PriorityClass(allocation.priority)))
    {
        AppendError(result.error, "priority: " + WindowsErrorText(GetLastError()));
    }

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    DWORD_PTR mask = 0;
    for (const uint32_t cpu : result.cpus)
    {
        if (cpu < sizeof(DWORD_PTR) * 8)
        {
            mask |= DWORD_PTR{1} << cpu;
        }
    }
    if (systemMask != 0)
    {
        mask &= systemMask;
    }
    if (mask == 0 || !SetProcessAffinityMask(GetCurrentProcess(), mask))
    {
        AppendError(result.error, "CPUs: " + (mask == 0 ? std::string("none of them exist") : WindowsErrorText(GetLastError())));
        // Report what the process actually runs on.
        GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
        result.cpus.clear();
        for (uint32_t cpu = 0; cpu < sizeof(DWORD_PTR) * 8; ++cpu)
        {
            if ((processMask >> cpu) & 1)
            {
                result.cpus.push_back(cpu);
            }
        }
    }
#else
    // On Linux this sets the calling thread's nice value; threads started later inherit it.
    if (setpriority(PRIO_PROCESS, 0, NiceValue(allocation.priority)) != 0)
    {
        AppendError(result.error, std::string("priority: ") + std::strerror(errno));
    }
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const uint32_t cpu : result.cpus)
    {
        if (cpu < CPU_SETSIZE)
        {
            CPU_SET(cpu, &set);
        }
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
    {
        AppendError(result.error, std::string("CPUs: ") + std::strerror(errno));
    }
#else
    if (result.cpus.size() != topology.processors.size())
    {
        AppendError(result.error, "CPUs: this OS cannot pin a process to processors");
        result.cpus = ResolveCpuSelection({}, topology);
    }
#endif
#endif
    return result;
}

std::string DescribeProcessAllocation(
    const ProcessAllocation& allocation,
    const ProcessAllocationResult& result,
    const ProcessorTopology& topology)
{
    std::string text = std::string(ProcessPriorityLabel(allocation.priority)) + " priority, CPUs " +
                       FormatCpuList(result.cpus) + " (" + std::to_string(result.cpus.size()) + " of " +
                       std::to_string(topology.processors.size()) + ")";
    if (!result.error.empty())
    {
        text += "; not applied: " + result.error;
    }
    return text;
}

std::string_view ProcessPriorityLabel(ProcessPriority priority)
{
    return kPriorityLabels[static_cast<size_t>(priority)];
}

std::string_view ProcessPriorityKey(ProcessPriority priority)
{
    return kPriorityKeys[static_cast<size_t>(priority)];
}

std::optional<ProcessPriority> ParseProcessPriority(std::string_view key)
{
    std::string normalized(key);
    std::replace(normalized.begin(), normalized.end(), '-', '_');
    for (size_t index = 0; index < std::size(kPriorityKeys); ++index)
    {
        if (normalized == kPriorityKeys[index])
        {
            return static_cast<ProcessPriority>(index);
        }
    }
    return std::nullopt;
}

std::string_view CpuSelectionKey(CpuSelection selection)
{
    return kSelectionKeys[static_cast<size_t>(selection)];
}

std::optional<CpuSelection> ParseCpuSelection(std::string_view key)
{
    for (size_t index = 0; index < std::size(kSelectionKeys); ++index)
    {
        if (key == kSelectionKeys[index])
        {
            return static_cast<CpuSelection>(index);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint32_t>> ParseCpuList(std::string_view text)
{
    // Far above any processor index; keeps a typo like 0-4000000000 from allocating forever.
    constexpr uint32_t kMaxIndex = 4095;
    const auto parseIndex = [](std::string_view part, uint32_t& value)
    {
        return !part.empty() &&
               std::from_chars(part.data(), part.data() + part.size(), value).ptr == part.data() + part.size() &&
               value <= kMaxIndex;
    };

    std::vector<uint32_t> cpus;
    while (!text.empty())
    {
        const size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (comma != std::string_view::npos && text.empty())
        {
            return std::nullopt;
        }

        const size_t dash = part.find('-');
        uint32_t first = 0;
        uint32_t last = 0;
        if (dash == std::string_view::npos)
        {
            if (!parseIndex(part, first))
            {
                return std::nullopt;
            }
            last = first;
        }
        else if (!parseIndex(part.substr(0, dash), first) || !parseIndex(part.substr(dash + 1), last) || last < first)
        {
            return std::nullopt;
        }
        for (uint32_t cpu = first; cpu <= last; ++cpu)
        {
            cpus.push_back(cpu);
        }
    }
    if (cpus.empty())
    {
        return std::nullopt;
    }
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return cpus;
}

std::string FormatCpuList(const std::vector<uint32_t>& cpus)
{
    std::string text;
    for (size_t index = 0; index < cpus.size();)
    {
        size_t end = index;
        while (end + 1 < cpus.size() && cpus[end + 1] == cpus[end] + 1)
        {
            ++end;
        }
        if (!text.empty())
        {
            text += ',';
        }
        text += std::to_string(cpus[index]);
        if (end > index)
        {
            text += '-' + std::to_string(cpus[end]);
        }
        index = end + 1;
    }
    return text;
}
}
}
