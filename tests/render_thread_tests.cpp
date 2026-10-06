#include <engine/core/threading/render_thread.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

// The work runs on another thread, which counts as render work; the caller's does not.
void WorkRunsOnTheRenderThread()
{
    RenderThread renderThread(RenderThread::Mode::Threaded);
    std::thread::id workThread;
    bool inRenderWork = false;
    renderThread.Submit([&]()
                        {
                            workThread = std::this_thread::get_id();
                            inRenderWork = RenderThread::IsRenderWork();
                        });
    renderThread.WaitIdle();
    Require(workThread != std::this_thread::get_id(), "the work must run on the render thread");
    Require(inRenderWork, "the render thread's work must count as render work");
    Require(!RenderThread::IsRenderWork(), "the submitting thread is not render work");
}

// Submit hands over the next frame only once the previous one finished: at most one frame apart.
void SubmitWaitsForThePreviousFrame()
{
    RenderThread renderThread(RenderThread::Mode::Threaded);
    std::atomic<bool> firstFinished = false;
    renderThread.Submit([&]()
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            firstFinished = true;
                        });
    // Returns at once: nothing was in hand.
    Require(!firstFinished, "the first frame must still be running when Submit returns");
    renderThread.Submit([]()
                        {
                        });
    Require(firstFinished, "Submit must wait for the previous frame");
}

// RunExclusive runs after every submitted frame, on the calling thread.
void ExclusiveRunsAfterTheSubmittedWork()
{
    RenderThread renderThread(RenderThread::Mode::Threaded);
    std::atomic<int> frames = 0;
    for (int frame = 0; frame < 5; ++frame)
    {
        renderThread.Submit([&]()
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                ++frames;
                            });
    }
    std::thread::id exclusiveThread;
    const int seen = renderThread.RunExclusive([&]()
                                               {
                                                   exclusiveThread = std::this_thread::get_id();
                                                   return frames.load();
                                               });
    Require(seen == 5, "RunExclusive must see every submitted frame done, saw " + std::to_string(seen));
    Require(exclusiveThread == std::this_thread::get_id(), "RunExclusive runs on the calling thread");
}

// A failure on the render thread comes back at the next Submit or WaitIdle, once.
void FailuresComeBackToTheCaller()
{
    RenderThread renderThread(RenderThread::Mode::Threaded);
    renderThread.Submit([]()
                        {
                            throw std::runtime_error("device lost");
                        });
    bool rethrown = false;
    try
    {
        renderThread.WaitIdle();
    }
    catch (const std::runtime_error& error)
    {
        rethrown = std::string(error.what()) == "device lost";
    }
    Require(rethrown, "WaitIdle must rethrow the render thread's failure");
    // Reported once: the thread goes on.
    bool ran = false;
    renderThread.Submit([&]()
                        {
                            ran = true;
                        });
    renderThread.WaitIdle();
    Require(ran, "work after a reported failure must run");

    renderThread.Submit([]()
                        {
                            throw std::runtime_error("out of memory");
                        });
    bool skipped = true;
    bool rethrownAtSubmit = false;
    try
    {
        renderThread.Submit([&]()
                            {
                                skipped = false;
                            });
    }
    catch (const std::runtime_error&)
    {
        rethrownAtSubmit = true;
    }
    renderThread.WaitIdle();
    Require(rethrownAtSubmit, "Submit must rethrow the previous frame's failure");
    Require(skipped, "the frame submitted after a failure must not run");
}

// Inline mode runs the work inside Submit, as render work.
void InlineModeRunsInSubmit()
{
    RenderThread renderThread(RenderThread::Mode::Inline);
    std::thread::id workThread;
    bool inRenderWork = false;
    renderThread.Submit([&]()
                        {
                            workThread = std::this_thread::get_id();
                            inRenderWork = RenderThread::IsRenderWork();
                        });
    Require(workThread == std::this_thread::get_id(), "inline work must run on the calling thread");
    Require(inRenderWork, "inline work must count as render work");
    Require(!RenderThread::IsRenderWork(), "after Submit the caller is not render work");
    bool threw = false;
    try
    {
        renderThread.Submit([]()
                            {
                                throw std::runtime_error("inline failure");
                            });
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "an inline failure must propagate from Submit");
}

// Destroying the thread finishes the frame in hand.
void DestructionFinishesTheFrameInHand()
{
    std::atomic<bool> finished = false;
    {
        RenderThread renderThread(RenderThread::Mode::Threaded);
        renderThread.Submit([&]()
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                finished = true;
                            });
    }
    Require(finished, "the frame in hand must finish before the thread stops");
}
}

int main()
{
    try
    {
        WorkRunsOnTheRenderThread();
        SubmitWaitsForThePreviousFrame();
        ExclusiveRunsAfterTheSubmittedWork();
        FailuresComeBackToTheCaller();
        InlineModeRunsInSubmit();
        DestructionFinishesTheFrameInHand();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "render thread tests passed\n";
    return 0;
}
