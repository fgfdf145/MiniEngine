#include "task_system.h"

#include "thread_setup.h"

#include <engine/core/log/log.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace me
{

namespace
{
std::unique_ptr<enki::TaskScheduler> g_scheduler;

// The workers past the active count, held where enkiTS puts an idle worker to sleep: out of tasks,
// with nothing of a task on its stack. A worker waiting inside a task (WaitforTask) never gets
// here, so parking cannot stall the work it was part of.
struct WorkerParking
{
    std::mutex mutex;
    std::condition_variable wake;
    // enkiTS numbers the initializing thread 0, then the external slots, then the workers.
    uint32_t firstWorker = 0;
    uint32_t workers = 0;
    std::atomic<uint32_t> active{0};
    bool stopping = false;
};
WorkerParking g_parking;

void OnWorkerStart(uint32_t threadNum)
{
    ConfigureCurrentThread(("Task worker " + std::to_string(threadNum)).c_str());
}

// enkiTS calls it as a worker goes to sleep for want of tasks. enkiTS counts the worker as asleep
// meanwhile, so a wake-up meant for it goes to a sleeping worker or to the next one to sleep.
void OnWorkerIdle(uint32_t threadNum)
{
    if (threadNum < g_parking.firstWorker)
    {
        return;
    }
    const uint32_t worker = threadNum - g_parking.firstWorker;
    std::unique_lock lock(g_parking.mutex);
    g_parking.wake.wait(lock, [worker]()
                        {
                            return g_parking.stopping || worker < g_parking.active.load(std::memory_order_relaxed);
                        });
}

class FunctionTaskSet final : public enki::ITaskSet
{
  public:
    explicit FunctionTaskSet(const std::function<void(uint32_t, uint32_t)>& body)
        : m_body(body)
    {
    }

    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override
    {
        // An exception must not leave a worker: enkiTS has no way to carry it.
        try
        {
            m_body(range.start, range.end);
        }
        catch (...)
        {
            const std::lock_guard lock(m_errorMutex);
            if (!m_error)
            {
                m_error = std::current_exception();
            }
        }
    }

    void RethrowError() const
    {
        if (m_error)
        {
            std::rethrow_exception(m_error);
        }
    }

  private:
    const std::function<void(uint32_t, uint32_t)>& m_body;
    std::mutex m_errorMutex;
    std::exception_ptr m_error;
};
}

void TaskSystem::Initialize(const Settings& settings)
{
    if (g_scheduler)
    {
        throw std::logic_error("The task system is already running");
    }
    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    enki::TaskSchedulerConfig config;
    config.numTaskThreadsToCreate = settings.workerThreads > 0 ? settings.workerThreads : WorkersForCpus(hardwareThreads);
    config.numExternalTaskThreads = settings.externalThreads;
    config.profilerCallbacks.threadStart = &OnWorkerStart;
    config.profilerCallbacks.waitForNewTaskSuspendStart = &OnWorkerIdle;
    {
        // Before the workers start: the first ones may go idle at once.
        const std::lock_guard lock(g_parking.mutex);
        g_parking.firstWorker = enki::TaskScheduler::GetNumFirstExternalTaskThread() + config.numExternalTaskThreads;
        g_parking.workers = config.numTaskThreadsToCreate;
        g_parking.active = settings.activeWorkerThreads > 0 ? std::min(settings.activeWorkerThreads, g_parking.workers) : g_parking.workers;
        g_parking.stopping = false;
    }
    g_scheduler = std::make_unique<enki::TaskScheduler>();
    g_scheduler->Initialize(config);
    LOG_INFO(
        "Task system: {} worker threads ({} active), {} external thread slots, {} hardware threads",
        config.numTaskThreadsToCreate,
        g_parking.active.load(),
        config.numExternalTaskThreads,
        hardwareThreads);
}

void TaskSystem::Shutdown()
{
    if (!g_scheduler)
    {
        return;
    }
    {
        // enkiTS waits for every worker to quit, the parked ones included.
        const std::lock_guard lock(g_parking.mutex);
        g_parking.stopping = true;
    }
    g_parking.wake.notify_all();
    g_scheduler->WaitforAllAndShutdown();
    g_scheduler.reset();
}

bool TaskSystem::IsRunning()
{
    return g_scheduler != nullptr;
}

enki::TaskScheduler& TaskSystem::Scheduler()
{
    if (!g_scheduler)
    {
        throw std::logic_error("The task system is not running");
    }
    return *g_scheduler;
}

uint32_t TaskSystem::ThreadCount()
{
    return g_scheduler ? g_scheduler->GetNumTaskThreads() : 1u;
}

uint32_t TaskSystem::ActiveThreadCount()
{
    if (!g_scheduler)
    {
        return 1u;
    }
    return g_scheduler->GetNumTaskThreads() - (g_parking.workers - g_parking.active.load(std::memory_order_relaxed));
}

uint32_t TaskSystem::WorkerThreadCount()
{
    return g_scheduler ? g_parking.workers : 0u;
}

uint32_t TaskSystem::WorkersForCpus(uint32_t cpus)
{
    return cpus > 2 ? cpus - 2 : 1u;
}

void TaskSystem::SetActiveWorkerThreads(uint32_t count)
{
    if (!g_scheduler)
    {
        return;
    }
    const uint32_t active = std::clamp(count, 1u, std::max(g_parking.workers, 1u));
    {
        const std::lock_guard lock(g_parking.mutex);
        if (g_parking.active.load(std::memory_order_relaxed) == active)
        {
            return;
        }
        g_parking.active.store(active, std::memory_order_relaxed);
    }
    // Lets the parked workers below the new count go; the ones above it park as they run dry.
    g_parking.wake.notify_all();
    LOG_INFO("Task system: {} of {} worker threads active", active, g_parking.workers);
}

bool TaskSystem::CanWaitOnCurrentThread()
{
    return g_scheduler && g_scheduler->GetThreadNum() != enki::NO_THREAD_NUM;
}

void TaskSystem::ParallelFor(
    uint32_t count,
    uint32_t minRange,
    const std::function<void(uint32_t begin, uint32_t end)>& body,
    TaskPriority priority)
{
    if (count == 0)
    {
        return;
    }
    minRange = std::max(minRange, 1u);
    if (count <= minRange || !CanWaitOnCurrentThread())
    {
        body(0, count);
        return;
    }
    FunctionTaskSet task(body);
    task.m_SetSize = count;
    task.m_MinRange = minRange;
    task.m_Priority = static_cast<enki::TaskPriority>(priority);
    g_scheduler->AddTaskSetToPipe(&task);
    // Not the lower priorities: a frame thread must not pick up a long background task here.
    g_scheduler->WaitforTask(&task, task.m_Priority);
    task.RethrowError();
}

ScopedExternalTaskThread::ScopedExternalTaskThread()
{
    if (g_scheduler)
    {
        m_registered = g_scheduler->RegisterExternalTaskThread();
        if (!m_registered)
        {
            LOG_WARN("No external task thread slot is left: this thread runs its parallel work alone");
        }
    }
}

ScopedExternalTaskThread::~ScopedExternalTaskThread()
{
    if (m_registered && g_scheduler)
    {
        g_scheduler->DeRegisterExternalTaskThread();
    }
}
}
