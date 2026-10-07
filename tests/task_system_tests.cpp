#include <engine/core/threading/task_system.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Notes the calling thread, and holds the first one back until a second thread has arrived (or a few
// seconds passed), so a loop over trivial work cannot finish on one thread before the others wake.
class ThreadRendezvous
{
  public:
    void Arrive()
    {
        std::unique_lock lock(m_mutex);
        m_threads.insert(std::this_thread::get_id());
        m_changed.notify_all();
        m_changed.wait_for(lock, std::chrono::seconds(5), [this]()
                           {
                               return m_threads.size() > 1;
                           });
    }
    size_t Count()
    {
        const std::lock_guard lock(m_mutex);
        return m_threads.size();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::set<std::thread::id> m_threads;
};

// Every index once, in ranges no longer than needed, on more than one thread.
void ParallelForCoversEveryIndexOnce()
{
    constexpr uint32_t kCount = 100000;
    std::vector<std::atomic<int>> visits(kCount);
    ThreadRendezvous threads;
    std::atomic<uint32_t> ranges = 0;
    TaskSystem::ParallelFor(kCount, 256, [&](uint32_t begin, uint32_t end)
                            {
                                ++ranges;
                                for (uint32_t index = begin; index < end; ++index)
                                {
                                    ++visits[index];
                                }
                                threads.Arrive();
                            });
    for (uint32_t index = 0; index < kCount; ++index)
    {
        Require(visits[index] == 1, "index " + std::to_string(index) + " visited " + std::to_string(visits[index].load()) + " times");
    }
    Require(ranges > 1 && ranges <= kCount / 256 * 2, "split into about count / minRange ranges, got " + std::to_string(ranges.load()));
    Require(threads.Count() > 1, "the loop ran on one thread only");
}

// A range's exception comes back from ParallelFor, after the other ranges ran.
void ParallelForRethrows()
{
    std::atomic<uint32_t> ran = 0;
    bool rethrown = false;
    try
    {
        TaskSystem::ParallelFor(64, 1, [&](uint32_t begin, uint32_t end)
                                {
                                    ran += end - begin;
                                    if (begin <= 7 && 7 < end)
                                    {
                                        throw std::runtime_error("range failed");
                                    }
                                });
    }
    catch (const std::runtime_error& error)
    {
        rethrown = std::string(error.what()) == "range failed";
    }
    Require(rethrown, "ParallelFor must rethrow a range's exception");
    Require(ran == 64, "every range must run, ran " + std::to_string(ran.load()));
}

// A thread the scheduler did not start runs its loops alone until it registers.
void ExternalThreadsRegister()
{
    bool before = true;
    bool registered = false;
    ThreadRendezvous threads;
    std::thread external([&]()
                         {
                             before = TaskSystem::CanWaitOnCurrentThread();
                             const ScopedExternalTaskThread scope;
                             registered = TaskSystem::CanWaitOnCurrentThread();
                             TaskSystem::ParallelFor(4096, 16, [&](uint32_t, uint32_t)
                                                     {
                                                         threads.Arrive();
                                                     });
                         });
    external.join();
    Require(!before, "an unregistered thread cannot wait for tasks");
    Require(registered, "a registered thread can wait for tasks");
    Require(threads.Count() > 1, "a registered thread's loop must spread over the workers");
}

// The threads that ran a loop of ranges long enough for every awake worker to join in.
std::set<std::thread::id> LoopThreads()
{
    std::mutex mutex;
    std::set<std::thread::id> threads;
    TaskSystem::ParallelFor(64, 1, [&](uint32_t, uint32_t)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                const std::lock_guard lock(mutex);
                                threads.insert(std::this_thread::get_id());
                            });
    return threads;
}

// Parked workers take no tasks; let go, they take them again. A worker parks once it runs dry, so
// a loop after the change wakes the workers that were asleep and they park after it.
void ParkedWorkersTakeNoTasks()
{
    const uint32_t workers = TaskSystem::WorkerThreadCount();
    TaskSystem::SetActiveWorkerThreads(1);
    Require(TaskSystem::ActiveThreadCount() == TaskSystem::ThreadCount() - (workers - 1), "one worker active");
    LoopThreads();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const size_t parkedThreads = LoopThreads().size();
    Require(parkedThreads <= 2, "the calling thread and one worker, got " + std::to_string(parkedThreads));

    TaskSystem::SetActiveWorkerThreads(0);
    Require(TaskSystem::ActiveThreadCount() == TaskSystem::ThreadCount() - (workers - 1), "the count is at least one");
    TaskSystem::SetActiveWorkerThreads(workers + 10);
    Require(TaskSystem::ActiveThreadCount() == TaskSystem::ThreadCount(), "the count is at most every worker");
    const size_t wokenThreads = LoopThreads().size();
    Require(wokenThreads > 2, "let go, the workers take tasks again, got " + std::to_string(wokenThreads));
}

// Without the scheduler the loop runs inline, in one range.
void InlineWithoutTheScheduler()
{
    uint32_t calls = 0;
    TaskSystem::ParallelFor(1000, 10, [&](uint32_t begin, uint32_t end)
                            {
                                ++calls;
                                Require(begin == 0 && end == 1000, "one range over everything");
                            });
    Require(calls == 1, "inline means one call");
}
}

int main()
{
    try
    {
        InlineWithoutTheScheduler();
        TaskSystem::Settings settings;
        settings.workerThreads = 4;
        settings.externalThreads = 1;
        TaskSystem::Initialize(settings);
        Require(TaskSystem::ThreadCount() == 6, "4 workers, the main thread and one external slot");
        Require(TaskSystem::CanWaitOnCurrentThread(), "the initializing thread is task thread 0");
        ParallelForCoversEveryIndexOnce();
        ParallelForRethrows();
        ExternalThreadsRegister();
        ParkedWorkersTakeNoTasks();
        TaskSystem::Shutdown();
        // Shutting down lets the parked workers go.
        settings.activeWorkerThreads = 1;
        TaskSystem::Initialize(settings);
        Require(TaskSystem::ActiveThreadCount() == 3, "one of 4 workers, the main thread and one external slot");
        ParallelForCoversEveryIndexOnce();
        TaskSystem::Shutdown();
        InlineWithoutTheScheduler();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "task system tests passed\n";
    return 0;
}
