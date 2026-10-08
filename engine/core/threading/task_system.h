#pragma once

#include <TaskScheduler.h>

#include <cstdint>
#include <functional>

namespace me
{

// enkiTS's three priorities, by what waits on the work.
enum class TaskPriority : uint8_t
{
    // Inside a frame: the frame waits for it (parallel loops, physics, command recording).
    High = enki::TASK_PRIORITY_HIGH,
    // Wanted soon, not by this frame (texture preparation, ray scene builds).
    Medium = enki::TASK_PRIORITY_MED,
    // Background work nobody waits for.
    Low = enki::TASK_PRIORITY_LOW,
};

// The engine's one task scheduler (enkiTS). Its workers run the parallel work of every subsystem,
// so the subsystems share the cores instead of each starting a pool of its own. The thread that
// calls Initialize is task thread 0; the render thread registers as an external one
// (ScopedExternalTaskThread).
class TaskSystem
{
  public:
    struct Settings
    {
        // Worker threads besides the initializing thread; 0 takes the logical processors less two,
        // which leaves one for the main thread and one for the render thread.
        uint32_t workerThreads = 0;
        // How many of them take tasks at first; 0 is all of them. See SetActiveWorkerThreads.
        uint32_t activeWorkerThreads = 0;
        // Threads the scheduler did not start that also add and wait for tasks.
        uint32_t externalThreads = 1;
    };

    static void Initialize(const Settings& settings);
    // With the default settings. Not a default argument: GCC and Clang read Settings' member
    // initializers only once TaskSystem is complete, and a default argument is not past that point.
    static void Initialize()
    {
        Initialize(Settings{});
    }
    // Waits for the tasks in flight, then stops the workers. Nothing may add tasks afterwards.
    static void Shutdown();
    static bool IsRunning();
    static enki::TaskScheduler& Scheduler();
    // The scheduler's threads, workers and registered external ones included. Thread numbers stay
    // below it, so it sizes per-thread storage.
    static uint32_t ThreadCount();
    // ThreadCount less the parked workers: how many threads can run tasks at once, for sizing work.
    static uint32_t ActiveThreadCount();
    static uint32_t WorkerThreadCount();
    // Lets the first count workers take tasks (clamped to 1..WorkerThreadCount) and parks the others
    // the next time they run out of work, so the work follows a change of the process's CPUs without
    // restarting the scheduler. A parked worker sleeps until a later call lets it run again.
    static void SetActiveWorkerThreads(uint32_t count);
    // The workers for a process given cpus logical processors: one each is left to the main thread
    // and the render thread.
    static uint32_t WorkersForCpus(uint32_t cpus);
    // True on the threads that may add and wait for tasks: task thread 0, the workers, and
    // registered external threads.
    static bool CanWaitOnCurrentThread();

    // Runs body(begin, end) over [0, count) in ranges of about minRange (enkiTS splits the set in
    // chunks first, so a chunk's last range can be shorter), on the workers and the calling thread,
    // and returns once every range has run. While waiting the calling thread runs tasks of this
    // priority and higher only. The first exception a range throws is
    // rethrown here, after the others finished. Runs inline when the scheduler is not running, the
    // calling thread cannot wait for tasks, or the work is a single range.
    static void ParallelFor(
        uint32_t count,
        uint32_t minRange,
        const std::function<void(uint32_t begin, uint32_t end)>& body,
        TaskPriority priority = TaskPriority::High);
};

// Registers the calling thread with the task scheduler for its lifetime, so it can run and wait for
// tasks. Does nothing when the scheduler is not running or has no external slot left.
class ScopedExternalTaskThread
{
  public:
    ScopedExternalTaskThread();
    ~ScopedExternalTaskThread();
    ScopedExternalTaskThread(const ScopedExternalTaskThread&) = delete;
    ScopedExternalTaskThread& operator=(const ScopedExternalTaskThread&) = delete;

  private:
    bool m_registered = false;
};
}
