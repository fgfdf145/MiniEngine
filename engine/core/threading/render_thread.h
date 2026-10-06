#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

namespace me
{

// The thread that does a frame's render work while the main thread builds the next frame. The two
// are at most one frame apart: Submit waits for the previous frame's work to finish before handing
// over the next. Inline mode runs each frame's work inside Submit instead, on the calling thread
// (--no-render-thread), for comparison and debugging.
class RenderThread
{
  public:
    enum class Mode
    {
        Threaded,
        Inline,
    };

    explicit RenderThread(Mode mode);
    // Finishes the work in hand, then stops the thread. A failure nobody collected is dropped.
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Hands over one frame's render work. Waits while the previous frame's runs. Rethrows the
    // failure of earlier work, in which case this work is not run.
    void Submit(std::function<void()> work);
    // Waits for every submitted frame's work to finish; rethrows its failure.
    void WaitIdle();
    // WaitIdle, then fn on the calling thread while the render thread waits for the next frame:
    // fn has the render thread's objects to itself (swapchain rebuilds, captures, shutdown).
    template <typename Fn>
    std::invoke_result_t<Fn> RunExclusive(Fn&& fn)
    {
        WaitIdle();
        return std::forward<Fn>(fn)();
    }

    Mode GetMode() const
    {
        return m_mode;
    }
    // True while running frame work: on the render thread, or inside Submit in inline mode.
    static bool IsRenderWork();

  private:
    void Loop();
    void ThrowIfCalledFromRenderWork() const;
    void RethrowPendingError();

    Mode m_mode;
    std::mutex m_mutex;
    // Signalled when work arrives or the thread is to stop.
    std::condition_variable m_workReady;
    // Signalled when the work in hand finished.
    std::condition_variable m_workDone;
    std::function<void()> m_work;
    bool m_busy = false;
    bool m_stopping = false;
    std::exception_ptr m_error;
    std::thread m_thread;
};
}
