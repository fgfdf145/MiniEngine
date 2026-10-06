#include "task_system.h"

#include "thread_setup.h"

#include <engine/core/log/log.h>

#include <algorithm>
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

void OnWorkerStart(uint32_t threadNum)
{
    ConfigureCurrentThread(("Task worker " + std::to_string(threadNum)).c_str());
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
    config.numTaskThreadsToCreate = settings.workerThreads > 0 ? settings.workerThreads : std::max(1u, hardwareThreads > 2 ? hardwareThreads - 2 : 1u);
    config.numExternalTaskThreads = settings.externalThreads;
    config.profilerCallbacks.threadStart = &OnWorkerStart;
    g_scheduler = std::make_unique<enki::TaskScheduler>();
    g_scheduler->Initialize(config);
    LOG_INFO(
        "Task system: {} worker threads, {} external thread slots, {} hardware threads",
        config.numTaskThreadsToCreate,
        config.numExternalTaskThreads,
        hardwareThreads);
}

void TaskSystem::Shutdown()
{
    if (!g_scheduler)
    {
        return;
    }
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
