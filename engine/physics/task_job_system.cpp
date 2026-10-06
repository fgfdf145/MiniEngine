#include "task_job_system.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace me
{

namespace
{
// How long a drainer waits for the next job before it ends: long enough to bridge the gap between
// a step's dependent jobs, short next to a frame.
constexpr std::chrono::microseconds kDrainerLinger{50};
}

TaskJobSystem::TaskJobSystem(uint32_t maxJobs, uint32_t maxBarriers, uint32_t drainers)
    : m_drainTasks(std::make_unique<DrainTask[]>(kDrainTaskCount))
{
    JobSystemWithBarrier::Init(maxBarriers);
    m_jobs.Init(maxJobs, maxJobs);
    // The stepping thread is one of the step's threads, so the drainers are the others.
    const uint32_t others = TaskSystem::ThreadCount() > 1 ? TaskSystem::ThreadCount() - 1 : 1;
    m_drainers = std::max(1u, std::min(drainers > 0 ? drainers : kDefaultDrainers, others));
    for (uint32_t index = 0; index < kDrainTaskCount; ++index)
    {
        m_drainTasks[index].owner = this;
        m_drainTasks[index].m_Priority = static_cast<enki::TaskPriority>(TaskPriority::High);
    }
}

TaskJobSystem::~TaskJobSystem()
{
    // Every step waited for its jobs, but a drain task may still be returning.
    for (uint32_t index = 0; index < kDrainTaskCount; ++index)
    {
        while (!m_drainTasks[index].GetIsComplete())
        {
            std::this_thread::yield();
        }
    }
}

int TaskJobSystem::GetMaxConcurrency() const
{
    return static_cast<int>(m_drainers + 1);
}

TaskJobSystem::JobHandle TaskJobSystem::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 numDependencies)
{
    // As JobSystemThreadPool: wait for a free job rather than fail.
    JPH::uint32 index;
    for (;;)
    {
        index = m_jobs.ConstructObject(name, color, this, function, numDependencies);
        if (index != JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex)
        {
            break;
        }
        JPH_ASSERT(false, "No jobs available!");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    Job* job = &m_jobs.Get(index);
    // The handle keeps a reference: the job may run and finish as soon as it is queued.
    JobHandle handle(job);
    if (numDependencies == 0)
    {
        QueueJob(job);
    }
    return handle;
}

void TaskJobSystem::QueueJob(Job* job)
{
    // The barrier runs it on the thread that waits for the step.
    if (!TaskSystem::CanWaitOnCurrentThread())
    {
        return;
    }
    if (Enqueue(job))
    {
        StartDrainTask();
    }
}

void TaskJobSystem::QueueJobs(Job** jobs, JPH::uint numJobs)
{
    if (!TaskSystem::CanWaitOnCurrentThread())
    {
        return;
    }
    bool start = false;
    for (JPH::uint index = 0; index < numJobs; ++index)
    {
        start |= Enqueue(jobs[index]);
    }
    if (start)
    {
        StartDrainTask();
    }
}

void TaskJobSystem::FreeJob(Job* job)
{
    m_jobs.DestructObject(job);
}

bool TaskJobSystem::Enqueue(Job* job)
{
    // The queue's reference, given back once a drainer ran the job.
    job->AddRef();
    const std::lock_guard lock(m_queueMutex);
    m_queue.push_back(job);
    m_queued.store(m_queue.size(), std::memory_order_release);
    if (m_activeDrainers > 0 || m_drainStarting)
    {
        return false;
    }
    m_drainStarting = true;
    return true;
}

void TaskJobSystem::StartDrainTask()
{
    // A free task: one enkiTS has completed. Drain tasks end within microseconds of their last job,
    // so the ring finds one at once.
    for (;;)
    {
        DrainTask& task = m_drainTasks[m_nextDrainTask];
        m_nextDrainTask = (m_nextDrainTask + 1) % kDrainTaskCount;
        if (task.GetIsComplete())
        {
            task.m_SetSize = m_drainers;
            task.m_MinRange = 1;
            TaskSystem::Scheduler().AddTaskSetToPipe(&task);
            return;
        }
    }
}

void TaskJobSystem::DrainTask::ExecuteRange(enki::TaskSetPartition, uint32_t)
{
    owner->Drain();
}

void TaskJobSystem::Drain()
{
    {
        const std::lock_guard lock(m_queueMutex);
        m_drainStarting = false;
        ++m_activeDrainers;
    }
    for (;;)
    {
        Job* job = nullptr;
        {
            const std::lock_guard lock(m_queueMutex);
            if (!m_queue.empty())
            {
                job = m_queue.front();
                m_queue.pop_front();
                m_queued.store(m_queue.size(), std::memory_order_release);
            }
        }
        if (job != nullptr)
        {
            // The barrier's thread may have run it already; Execute runs a job once.
            job->Execute();
            job->Release();
            continue;
        }
        // Nothing queued: wait a moment for the step's next jobs before ending.
        const auto lingerEnd = std::chrono::steady_clock::now() + kDrainerLinger;
        while (m_queued.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < lingerEnd)
        {
            std::this_thread::yield();
        }
        const std::lock_guard lock(m_queueMutex);
        if (m_queue.empty())
        {
            --m_activeDrainers;
            return;
        }
    }
}
}
