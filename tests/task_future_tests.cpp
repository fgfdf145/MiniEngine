#include <engine/core/threading/task_future.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
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

// The result comes back once; the future is empty afterwards.
void ReturnsTheResult()
{
    TaskFuture<int> future = RunAsync(TaskPriority::Medium, []()
                                      {
                                          return 42;
                                      });
    Require(future.valid(), "a started task has a future");
    Require(future.get() == 42, "get returns the result");
    Require(!future.valid(), "get empties the future");

    // Move-only captures and results.
    auto owned = std::make_unique<int>(7);
    TaskFuture<std::unique_ptr<int>> moved = RunAsync(TaskPriority::Low, [owned = std::move(owned)]() mutable
                                                      {
                                                          return std::move(owned);
                                                      });
    Require(*moved.get() == 7, "a move-only result comes back");
}

// The task's exception comes back from get.
void RethrowsTheException()
{
    TaskFuture<void> future = RunAsync(TaskPriority::High, []()
                                       {
                                           throw std::runtime_error("decode failed");
                                       });
    bool rethrown = false;
    try
    {
        future.get();
    }
    catch (const std::runtime_error& error)
    {
        rethrown = std::string(error.what()) == "decode failed";
    }
    Require(rethrown, "get must rethrow the task's exception");
}

// wait_for reports timeout while the task runs and ready once it finished.
void WaitForReportsProgress()
{
    std::atomic<bool> release = false;
    TaskFuture<void> future = RunAsync(TaskPriority::Low, [&]()
                                       {
                                           while (!release)
                                           {
                                               std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                           }
                                       });
    Require(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout, "a running task is not ready");
    release = true;
    Require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "a finished task is ready");
}

// Destroying an unfinished future waits for its task, as std::async's does.
void DestructionWaits()
{
    std::atomic<bool> finished = false;
    {
        TaskFuture<void> future = RunAsync(TaskPriority::Low, [&]()
                                           {
                                               std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                               finished = true;
                                           });
    }
    Require(finished, "the destructor must wait for the task");

    // Assigning over a running one waits for it too.
    finished = false;
    TaskFuture<void> future = RunAsync(TaskPriority::Low, [&]()
                                       {
                                           std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                           finished = true;
                                       });
    future = TaskFuture<void>{};
    Require(finished, "move assignment must wait for the task it replaces");
}

// The task runs on a worker; from a thread the scheduler takes no tasks from, on a thread of its own.
void RunsOnAnotherThread(bool expectWorker)
{
    const std::thread::id caller = std::this_thread::get_id();
    TaskFuture<std::thread::id> future = RunAsync(TaskPriority::Medium, []()
                                                  {
                                                      return std::this_thread::get_id();
                                                  });
    Require(future.get() != caller, "the task must not run on the calling thread");
    Require(TaskSystem::CanWaitOnCurrentThread() == expectWorker, "registration as expected");
}

// Many tasks at once, each with its result.
void ManyTasks()
{
    std::vector<TaskFuture<int>> futures;
    for (int index = 0; index < 500; ++index)
    {
        futures.push_back(RunAsync(TaskPriority::Medium, [index]()
                                   {
                                       return index * 2;
                                   }));
    }
    for (int index = 0; index < 500; ++index)
    {
        Require(futures[static_cast<size_t>(index)].get() == index * 2, "task " + std::to_string(index) + " returned another's result");
    }
}

void RunAll(bool expectWorker)
{
    ReturnsTheResult();
    RethrowsTheException();
    WaitForReportsProgress();
    DestructionWaits();
    RunsOnAnotherThread(expectWorker);
    ManyTasks();
}
}

int main()
{
    try
    {
        // Without the scheduler every task gets a thread of its own.
        RunAll(false);
        TaskSystem::Settings settings;
        settings.workerThreads = 4;
        TaskSystem::Initialize(settings);
        RunAll(true);
        // An unregistered thread still gets its tasks run.
        std::exception_ptr error;
        std::thread outsider([&]()
                             {
                                 try
                                 {
                                     RunAll(false);
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
        TaskSystem::Shutdown();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "task future tests passed\n";
    return 0;
}
