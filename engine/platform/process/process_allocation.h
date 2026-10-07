#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

namespace platform::process
{
// The scheduling class the OS gives the whole process. Realtime is left out: it starves the input and
// audio threads of the OS itself.
enum class ProcessPriority : uint8_t
{
    BelowNormal,
    Normal,
    AboveNormal,
    High,
};

// Which logical processors the process may run on.
enum class CpuSelection : uint8_t
{
    All,
    // The fastest efficiency class of a hybrid CPU (Intel's P-cores); every processor elsewhere.
    Performance,
    // The processors listed in ProcessAllocation::customCpus.
    Custom,
};

// The Preferences window's Process section, and the --priority / --cpus options.
struct ProcessAllocation
{
    bool operator==(const ProcessAllocation&) const = default;

    ProcessPriority priority = ProcessPriority::High;
    CpuSelection cpus = CpuSelection::All;
    // With Custom: logical processor indices, numbered as Task Manager numbers them.
    std::vector<uint32_t> customCpus;
};

struct LogicalProcessor
{
    uint32_t index = 0;
    // The physical core; simultaneous multithreading siblings share it.
    uint32_t core = 0;
    // Higher is faster. The same on every processor of a CPU that is not hybrid.
    uint8_t efficiencyClass = 0;
};

struct ProcessorTopology
{
    // Ascending by index.
    std::vector<LogicalProcessor> processors;

    bool IsHybrid() const;
    bool IsPerformance(const LogicalProcessor& processor) const;
    uint32_t PerformanceCount() const;
};

// The logical processors the process can be given. On Windows, those of processor group 0 only: a
// process affinity mask covers one group, which holds every processor of a machine with up to 64.
ProcessorTopology QueryProcessorTopology();

// The processors a selection names on this topology, ascending. Indices the topology lacks are
// dropped; a selection that names none falls back to every processor.
std::vector<uint32_t> ResolveCpuSelection(const ProcessAllocation& allocation, const ProcessorTopology& topology);

struct ProcessAllocationResult
{
    // The processors the process now runs on.
    std::vector<uint32_t> cpus;
    // Empty when the priority and the processors both took effect.
    std::string error;
};

// Sets the process's priority class and processor affinity. Affinity covers every thread of the
// process, those already running included, and the threads it starts later inherit it.
ProcessAllocationResult ApplyProcessAllocation(const ProcessAllocation& allocation, const ProcessorTopology& topology);
// "High priority, CPUs 0-7 (8 of 24)", and what failed: for the log and the Preferences window.
std::string DescribeProcessAllocation(
    const ProcessAllocation& allocation,
    const ProcessAllocationResult& result,
    const ProcessorTopology& topology);

// "High", "Below Normal": for the UI.
std::string_view ProcessPriorityLabel(ProcessPriority priority);
// "high", "below_normal": for the settings file.
std::string_view ProcessPriorityKey(ProcessPriority priority);
// Takes a key; dashes may stand for the underscores (the command line's spelling).
std::optional<ProcessPriority> ParseProcessPriority(std::string_view key);
std::string_view CpuSelectionKey(CpuSelection selection);
std::optional<CpuSelection> ParseCpuSelection(std::string_view key);

// "0,2,4-7" into ascending, distinct indices; nullopt when malformed or empty.
std::optional<std::vector<uint32_t>> ParseCpuList(std::string_view text);
// The other way: ascending indices as "0-7,12".
std::string FormatCpuList(const std::vector<uint32_t>& cpus);
}
}
