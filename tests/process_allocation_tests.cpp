#include <engine/platform/process/process_allocation.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;
using namespace me::platform::process;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Eight performance CPUs (class 1) and sixteen efficiency ones (class 0), as an Intel 285K has.
ProcessorTopology HybridTopology()
{
    ProcessorTopology topology;
    for (uint32_t index = 0; index < 24; ++index)
    {
        topology.processors.push_back({index, index, static_cast<uint8_t>(index < 8 ? 1 : 0)});
    }
    return topology;
}

void TestCpuListsParseAndFormat()
{
    Require(ParseCpuList("0,2,4-7") == std::vector<uint32_t>{0, 2, 4, 5, 6, 7}, "a list with a range parses");
    Require(ParseCpuList("7,1-2,1") == std::vector<uint32_t>{1, 2, 7}, "a list comes back sorted and distinct");
    for (const char* bad : {"", ",", "1,", "a", "3-1", "1-", "-1", "0-99999"})
    {
        Require(!ParseCpuList(bad).has_value(), std::string("a malformed list is refused: '") + bad + "'");
    }
    Require(FormatCpuList({0, 1, 2, 3, 5, 8, 9}) == "0-3,5,8-9", "runs format as ranges");
    Require(FormatCpuList({4}) == "4", "one CPU formats alone");
}

void TestPriorityAndSelectionKeys()
{
    Require(ParseProcessPriority("above-normal") == ProcessPriority::AboveNormal, "the command line's dashes parse");
    Require(ParseProcessPriority("below_normal") == ProcessPriority::BelowNormal, "the settings file's keys parse");
    Require(!ParseProcessPriority("realtime").has_value(), "realtime is not offered");
    for (const ProcessPriority priority :
         {ProcessPriority::BelowNormal, ProcessPriority::Normal, ProcessPriority::AboveNormal, ProcessPriority::High})
    {
        Require(ParseProcessPriority(ProcessPriorityKey(priority)) == priority, "every priority's key parses back");
    }
    for (const CpuSelection selection : {CpuSelection::All, CpuSelection::Performance, CpuSelection::Custom})
    {
        Require(ParseCpuSelection(CpuSelectionKey(selection)) == selection, "every selection's key parses back");
    }
    Require(ProcessAllocation{}.priority == ProcessPriority::High, "the engine runs at high priority by default");
    Require(ProcessAllocation{}.cpus == CpuSelection::All, "the engine runs on every CPU by default");
}

void TestSelectionsResolveOnAHybridCpu()
{
    const ProcessorTopology topology = HybridTopology();
    Require(topology.IsHybrid() && topology.PerformanceCount() == 8, "the hybrid topology has eight performance CPUs");

    ProcessAllocation allocation;
    Require(ResolveCpuSelection(allocation, topology).size() == 24, "All takes every CPU");
    allocation.cpus = CpuSelection::Performance;
    Require(ResolveCpuSelection(allocation, topology) == std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7},
            "Performance takes the fastest class");
    allocation.cpus = CpuSelection::Custom;
    allocation.customCpus = {3, 9, 40};
    Require(ResolveCpuSelection(allocation, topology) == std::vector<uint32_t>{3, 9}, "Custom drops CPUs the machine lacks");
    allocation.customCpus = {40};
    Require(ResolveCpuSelection(allocation, topology).size() == 24, "a Custom set naming none falls back to all");

    ProcessorTopology uniform;
    for (uint32_t index = 0; index < 4; ++index)
    {
        uniform.processors.push_back({index, index / 2, 0});
    }
    allocation.cpus = CpuSelection::Performance;
    Require(!uniform.IsHybrid() && ResolveCpuSelection(allocation, uniform).size() == 4,
            "without efficiency cores Performance is every CPU");
}

// Sets this process's priority and CPUs for real, checks the OS took them, and puts them back.
void TestAllocationAppliesToTheProcess()
{
    const ProcessorTopology topology = QueryProcessorTopology();
    Require(!topology.processors.empty(), "the machine has processors");
    std::cout << "[process] " << topology.processors.size() << " CPUs, " << topology.PerformanceCount()
              << " performance, hybrid " << (topology.IsHybrid() ? "yes" : "no") << '\n';

    // A thread already running, as the physics and task workers are when the Preferences change.
    std::atomic<bool> release = false;
    std::thread running([&release]()
                        {
                            while (!release.load())
                            {
                                std::this_thread::yield();
                            }
                        });

    ProcessAllocation allocation;
    allocation.priority = ProcessPriority::AboveNormal;
    allocation.cpus = CpuSelection::Custom;
    allocation.customCpus = {topology.processors.front().index};
    ProcessAllocationResult result = ApplyProcessAllocation(allocation, topology);
    std::cout << "[process] " << DescribeProcessAllocation(allocation, result, topology) << '\n';
    Require(result.error.empty(), "the allocation applies: " + result.error);
    Require(result.cpus == allocation.customCpus, "the process runs on the one CPU");
#ifdef _WIN32
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    Require(processMask == (DWORD_PTR{1} << topology.processors.front().index), "Windows holds the affinity mask");
    Require(GetPriorityClass(GetCurrentProcess()) == ABOVE_NORMAL_PRIORITY_CLASS, "Windows holds the priority class");
    GROUP_AFFINITY threadAffinity{};
    Require(GetThreadGroupAffinity(running.native_handle(), &threadAffinity) != FALSE, "the running thread's affinity reads");
    Require(threadAffinity.Mask == processMask, "a thread already running moves to the process's CPUs");
#endif
    release = true;
    running.join();

    result = ApplyProcessAllocation(ProcessAllocation{}, topology);
    Require(result.error.empty(), "the default allocation applies: " + result.error);
    Require(result.cpus.size() == topology.processors.size(), "the default runs on every CPU");
#ifdef _WIN32
    Require(GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS, "the default is the high priority class");
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
#endif
}
}

int main()
{
    try
    {
        TestCpuListsParseAndFormat();
        TestPriorityAndSelectionKeys();
        TestSelectionsResolveOnAHybridCpu();
        TestAllocationAppliesToTheProcess();
    }
    catch (const std::exception& error)
    {
        std::cerr << "process_allocation_tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "process_allocation_tests passed\n";
    return 0;
}
