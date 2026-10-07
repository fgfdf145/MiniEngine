#include <engine/physics/task_job_system.h>

#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/PhysicsSettings.h>

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

// What a physics step does: jobs, some depending on others, all in one barrier, waited for. Returns
// how many threads ran them. With waitForSpread the first job holds on until a second thread runs
// one (or a few seconds passed), so trivial jobs cannot all finish on one thread before the others
// wake.
size_t RunJobs(TaskJobSystem& jobSystem, uint32_t jobCount, bool waitForSpread)
{
    std::atomic<uint32_t> independentRuns = 0;
    std::atomic<uint32_t> dependentRuns = 0;
    std::atomic<bool> orderBroken = false;
    std::mutex threadsMutex;
    std::condition_variable threadsChanged;
    std::set<std::thread::id> threads;
    const auto noteThread = [&]()
    {
        std::unique_lock lock(threadsMutex);
        threads.insert(std::this_thread::get_id());
        threadsChanged.notify_all();
        if (waitForSpread)
        {
            threadsChanged.wait_for(lock, std::chrono::seconds(5), [&]()
                                    {
                                        return threads.size() > 1;
                                    });
        }
    };

    JPH::JobSystem::Barrier* barrier = jobSystem.CreateBarrier();
    std::vector<JPH::JobHandle> handles;
    // Each dependent job waits for one independent job, which removes the dependency when done.
    for (uint32_t index = 0; index < jobCount; ++index)
    {
        JPH::JobHandle dependent = jobSystem.CreateJob("dependent", JPH::Color::sRed, [&, index]()
                                                       {
                                                           noteThread();
                                                           if (independentRuns.load() == 0)
                                                           {
                                                               orderBroken = true;
                                                           }
                                                           ++dependentRuns;
                                                           (void)index;
                                                       },
                                                       1);
        JPH::JobHandle independent = jobSystem.CreateJob("independent", JPH::Color::sGreen, [&, dependent]()
                                                         {
                                                             noteThread();
                                                             // Some work, so the jobs spread over threads.
                                                             volatile double sink = 0.0;
                                                             for (int step = 0; step < 20000; ++step)
                                                             {
                                                                 sink = sink + step * 0.5;
                                                             }
                                                             ++independentRuns;
                                                             dependent.RemoveDependency();
                                                         });
        handles.push_back(independent);
        handles.push_back(dependent);
    }
    barrier->AddJobs(handles.data(), static_cast<JPH::uint>(handles.size()));
    jobSystem.WaitForJobs(barrier);
    jobSystem.DestroyBarrier(barrier);

    Require(independentRuns == jobCount, "every independent job must run once, ran " + std::to_string(independentRuns.load()));
    Require(dependentRuns == jobCount, "every dependent job must run once, ran " + std::to_string(dependentRuns.load()));
    Require(!orderBroken, "a dependent job ran before its dependency");
    for (const JPH::JobHandle& handle : handles)
    {
        Require(handle.IsDone(), "every job must be done after the barrier");
    }
    return threads.size();
}
}

int main()
{
    JPH::RegisterDefaultAllocator();
    try
    {
        TaskSystem::Settings settings;
        settings.workerThreads = 4;
        TaskSystem::Initialize(settings);
        {
            TaskJobSystem jobSystem(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
            // The stepping thread and its drainers, no more than the task system has.
            Require(jobSystem.GetMaxConcurrency() >= 2 && jobSystem.GetMaxConcurrency() <= static_cast<int>(TaskSystem::ThreadCount()),
                    "concurrency is the drainers and the stepping thread");
            const size_t threads = RunJobs(jobSystem, 400, true);
            Require(threads > 1, "the jobs must spread over the task system's threads");
            // Again, reusing the jobs and tasks.
            for (int round = 0; round < 20; ++round)
            {
                RunJobs(jobSystem, 300, false);
            }

            // Fewer CPUs: a burst starts fewer drainers, while Jolt keeps the concurrency it sized its
            // jobs by.
            const int concurrency = jobSystem.GetMaxConcurrency();
            const uint32_t drainers = jobSystem.ActiveDrainers();
            TaskSystem::SetActiveWorkerThreads(1);
            Require(jobSystem.ActiveDrainers() == TaskSystem::ActiveThreadCount() - 1 && jobSystem.ActiveDrainers() < drainers,
                    "the drainers follow the active workers");
            Require(jobSystem.GetMaxConcurrency() == concurrency, "the concurrency stays");
            for (int round = 0; round < 10; ++round)
            {
                RunJobs(jobSystem, 300, false);
            }
            TaskSystem::SetActiveWorkerThreads(TaskSystem::WorkerThreadCount());
            Require(jobSystem.ActiveDrainers() == drainers, "more CPUs bring the drainers back");

            // Stepped from a thread the scheduler takes no tasks from: the barrier runs every job there.
            std::exception_ptr error;
            size_t outsiderThreads = 0;
            std::thread outsider([&]()
                                 {
                                     try
                                     {
                                         outsiderThreads = RunJobs(jobSystem, 200, false);
                                     }
                                     catch (...)
                                     {
                                         error = std::current_exception();
                                     }
                                 });
            outsider.join();
            if (error)
            {
                std::rethrow_exception(error);
            }
            Require(outsiderThreads == 1, "an unregistered thread's step runs on that thread alone");
        }
        TaskSystem::Shutdown();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "task job system tests passed\n";
    return 0;
}
