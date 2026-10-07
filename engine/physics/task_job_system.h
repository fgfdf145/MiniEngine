#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <engine/core/threading/task_system.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace me
{

// Jolt's jobs run on the engine's task system, so a physics step shares its workers instead of
// running a thread pool of its own. Not one task per job: enkiTS wakes every idle worker for each
// task it is given, and a step queues hundreds of jobs of microseconds each, which made physics
// two and a half times slower with 22 workers. Jobs go to a queue here instead, emptied by a few
// drain tasks, started once a burst and kept briefly between its jobs. A job that becomes ready on a
// thread the scheduler takes no tasks from is left to its barrier, which runs it on the thread
// waiting for the step.
class TaskJobSystem final : public JPH::JobSystemWithBarrier
{
  public:
    // drainers: how many threads besides the stepping one run a step's jobs at most; 0 takes
    // kDefaultDrainers, fewer when the task system has fewer threads. A burst starts no more of them
    // than the task system has active threads, so a step follows a change of the process's CPUs.
    TaskJobSystem(uint32_t maxJobs, uint32_t maxBarriers, uint32_t drainers = 0);
    ~TaskJobSystem() override;

    // One car and a static world: measured (GTA SA map, 2026-10-06), a step gains nothing past a
    // handful of threads while every extra drainer costs its wake-up.
    static constexpr uint32_t kDefaultDrainers = 7;

    // Fixed for the job system's life: Jolt reads it several times a step and sizes its jobs by it.
    int GetMaxConcurrency() const override;
    // The drainers a burst starts now.
    uint32_t ActiveDrainers() const;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 numDependencies = 0) override;

  protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint numJobs) override;
    void FreeJob(Job* job) override;

  private:
    // Runs queued jobs until none came for a moment; each range of a drain task runs one.
    class DrainTask final : public enki::ITaskSet
    {
      public:
        void ExecuteRange(enki::TaskSetPartition, uint32_t) override;

        TaskJobSystem* owner = nullptr;
    };

    void Drain();
    // Queues under the lock and returns whether a drain task must be started.
    bool Enqueue(Job* job);
    void StartDrainTask();

    JPH::FixedSizeFreeList<Job> m_jobs;
    uint32_t m_drainers = 1;

    std::mutex m_queueMutex;
    std::deque<Job*> m_queue;
    // For the drainers' spin: whether anything is queued, without the lock.
    std::atomic<size_t> m_queued{0};
    // Drainers running, and whether a drain task was started that has not reached one yet.
    uint32_t m_activeDrainers = 0;
    bool m_drainStarting = false;

    // Started drain tasks; one is reused once enkiTS completed it.
    static constexpr uint32_t kDrainTaskCount = 64;
    std::unique_ptr<DrainTask[]> m_drainTasks;
    uint32_t m_nextDrainTask = 0;
};
}
