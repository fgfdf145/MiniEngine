#pragma once

#include "task_system.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace me
{

namespace detail
{
// What a TaskFuture shares with its task: the result, and whether the task is done with it.
template <typename T>
class AsyncTaskState : public enki::ITaskSet
{
  public:
    ~AsyncTaskState() override = default;

    void ExecuteRange(enki::TaskSetPartition, uint32_t) override
    {
        Run();
    }

    // Runs the work on a thread of its own: the calling thread could not hand it to the scheduler.
    void StartThread()
    {
        m_thread = std::thread([this]()
                               {
                                   Run();
                               });
    }

    void MarkScheduled()
    {
        m_scheduled = true;
    }

    bool WaitFor(std::chrono::nanoseconds timeout)
    {
        {
            std::unique_lock lock(m_mutex);
            if (!m_readyChanged.wait_for(lock, timeout, [this]()
                                         {
                                             return m_ready;
                                         }))
            {
                return false;
            }
        }
        FinishWithTask();
        return true;
    }

    // Blocks without running other tasks: a frame thread waiting here must not pick up a long
    // background task instead.
    void Wait()
    {
        {
            std::unique_lock lock(m_mutex);
            m_readyChanged.wait(lock, [this]()
                                {
                                    return m_ready;
                                });
        }
        FinishWithTask();
    }

    T Take()
    {
        Wait();
        if (m_error)
        {
            std::rethrow_exception(m_error);
        }
        if constexpr (!std::is_void_v<T>)
        {
            return std::move(*m_value);
        }
    }

  protected:
    virtual void Invoke() = 0;

    std::optional<std::conditional_t<std::is_void_v<T>, char, T>> m_value;

  private:
    void Run()
    {
        try
        {
            Invoke();
        }
        catch (...)
        {
            m_error = std::current_exception();
        }
        {
            const std::lock_guard lock(m_mutex);
            m_ready = true;
        }
        m_readyChanged.notify_all();
    }

    // The scheduler touches the task a moment after the work signalled; the state may only go once
    // it is through.
    void FinishWithTask()
    {
        if (m_scheduled)
        {
            while (!GetIsComplete())
            {
                std::this_thread::yield();
            }
        }
        else if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_readyChanged;
    bool m_ready = false;
    bool m_scheduled = false;
    std::exception_ptr m_error;
    std::thread m_thread;
};

template <typename T, typename Fn>
class AsyncTaskStateFor final : public AsyncTaskState<T>
{
  public:
    explicit AsyncTaskStateFor(Fn&& fn)
        : m_fn(std::move(fn))
    {
    }

  private:
    void Invoke() override
    {
        if constexpr (std::is_void_v<T>)
        {
            m_fn();
        }
        else
        {
            this->m_value.emplace(m_fn());
        }
    }

    Fn m_fn;
};
}

// A task's result, as std::future holds an std::async call's (the same lower-case calls, so either
// fits the same code), and like it waits for the task when destroyed before it finished. Waiting
// never runs other tasks on the waiting thread.
template <typename T>
class TaskFuture
{
  public:
    TaskFuture() = default;
    explicit TaskFuture(std::unique_ptr<detail::AsyncTaskState<T>> state)
        : m_state(std::move(state))
    {
    }
    TaskFuture(TaskFuture&& other) noexcept = default;
    TaskFuture& operator=(TaskFuture&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_state = std::move(other.m_state);
        }
        return *this;
    }
    TaskFuture(const TaskFuture&) = delete;
    TaskFuture& operator=(const TaskFuture&) = delete;
    ~TaskFuture()
    {
        Reset();
    }

    bool valid() const
    {
        return m_state != nullptr;
    }
    void wait() const
    {
        m_state->Wait();
    }
    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout) const
    {
        return m_state->WaitFor(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout)) ? std::future_status::ready
                                                                                               : std::future_status::timeout;
    }
    // The result, or the task's exception; the future is empty afterwards.
    T get()
    {
        std::unique_ptr<detail::AsyncTaskState<T>> state = std::move(m_state);
        return state->Take();
    }

  private:
    void Reset()
    {
        if (m_state)
        {
            m_state->Wait();
            m_state.reset();
        }
    }

    std::unique_ptr<detail::AsyncTaskState<T>> m_state;
};

// Runs fn as a task (std::async on the task system). On a thread that cannot add tasks (one the
// scheduler neither started nor registered) it gets a thread of its own, as std::async gave it.
template <typename Fn>
TaskFuture<std::invoke_result_t<std::decay_t<Fn>&>> RunAsync(TaskPriority priority, Fn&& fn)
{
    using Result = std::invoke_result_t<std::decay_t<Fn>&>;
    auto state = std::make_unique<detail::AsyncTaskStateFor<Result, std::decay_t<Fn>>>(std::decay_t<Fn>(std::forward<Fn>(fn)));
    if (TaskSystem::CanWaitOnCurrentThread())
    {
        state->m_Priority = static_cast<enki::TaskPriority>(priority);
        state->MarkScheduled();
        TaskSystem::Scheduler().AddTaskSetToPipe(state.get());
    }
    else
    {
        state->StartThread();
    }
    return TaskFuture<Result>(std::move(state));
}
}
