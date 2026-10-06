#include "render_thread.h"

#include "task_system.h"
#include "thread_setup.h"

#include <stdexcept>
#include <utility>

namespace me
{

namespace
{
thread_local bool t_inRenderWork = false;

// Marks the calling thread as running frame work for the scope.
class RenderWorkScope
{
  public:
    RenderWorkScope()
        : m_previous(t_inRenderWork)
    {
        t_inRenderWork = true;
    }
    ~RenderWorkScope()
    {
        t_inRenderWork = m_previous;
    }

  private:
    bool m_previous;
};
}

RenderThread::RenderThread(Mode mode)
    : m_mode(mode)
{
    if (m_mode == Mode::Threaded)
    {
        m_thread = std::thread([this]()
                               {
                                   Loop();
                               });
    }
}

RenderThread::~RenderThread()
{
    if (m_thread.joinable())
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_workReady.notify_one();
        m_thread.join();
    }
}

void RenderThread::Submit(std::function<void()> work)
{
    ThrowIfCalledFromRenderWork();
    if (m_mode == Mode::Inline)
    {
        RethrowPendingError();
        const RenderWorkScope scope;
        work();
        return;
    }
    std::unique_lock lock(m_mutex);
    m_workDone.wait(lock, [this]()
                    {
                        return !m_busy;
                    });
    if (m_error)
    {
        std::exception_ptr error = std::exchange(m_error, nullptr);
        std::rethrow_exception(error);
    }
    m_work = std::move(work);
    m_busy = true;
    lock.unlock();
    m_workReady.notify_one();
}

void RenderThread::WaitIdle()
{
    ThrowIfCalledFromRenderWork();
    if (m_mode == Mode::Inline)
    {
        RethrowPendingError();
        return;
    }
    std::unique_lock lock(m_mutex);
    m_workDone.wait(lock, [this]()
                    {
                        return !m_busy;
                    });
    if (m_error)
    {
        std::exception_ptr error = std::exchange(m_error, nullptr);
        std::rethrow_exception(error);
    }
}

bool RenderThread::IsRenderWork()
{
    return t_inRenderWork;
}

void RenderThread::ThrowIfCalledFromRenderWork() const
{
    // The render thread would wait for itself.
    if (m_mode == Mode::Threaded && t_inRenderWork)
    {
        throw std::logic_error("Render work must not submit work or wait for the render thread");
    }
}

void RenderThread::RethrowPendingError()
{
    if (m_error)
    {
        std::exception_ptr error = std::exchange(m_error, nullptr);
        std::rethrow_exception(error);
    }
}

void RenderThread::Loop()
{
    ConfigureCurrentThread("Render");
    // The render thread runs parallel loops of its own and waits for them.
    const ScopedExternalTaskThread taskThread;
    const RenderWorkScope scope;
    std::unique_lock lock(m_mutex);
    while (true)
    {
        m_workReady.wait(lock, [this]()
                         {
                             return m_stopping || m_busy;
                         });
        if (!m_busy)
        {
            return;
        }
        std::function<void()> work = std::move(m_work);
        m_work = nullptr;
        lock.unlock();
        std::exception_ptr error;
        try
        {
            work();
        }
        catch (...)
        {
            error = std::current_exception();
        }
        // The work's captures go before the main thread is told it finished: they may own the
        // frame's data, which the main thread reuses next.
        work = nullptr;
        lock.lock();
        if (error && !m_error)
        {
            m_error = error;
        }
        m_busy = false;
        m_workDone.notify_all();
    }
}
}
