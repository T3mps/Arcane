#include <Arcane/Base/ServiceThread.hpp>

#include <Arcane/Base/Diagnostics.hpp>   // GuaranteeStackForThisThread -- spec S5.1 item 4 (crash window plan 1, R23)
#include <Arcane/Base/Log.hpp>

#include <utility>

namespace Arcane
{
    ServiceThread::ServiceThread(std::string debugName,
                                 std::function<void()> main,
                                 std::function<void()> wake)
        : m_debugName(std::move(debugName)), m_wake(std::move(wake))
    {
        m_thread = std::thread([this, body = std::move(main)]
        {
            // FIRST statement of the thread body, before `body` can grow a
            // single stack frame: a service thread that overflows its stack
            // must still be able to run the exception filter, and the filter
            // needs 64 KiB below the guard page to get that far. See
            // Diagnostics::GuaranteeStackForThisThread.
            Arcane::Diagnostics::GuaranteeStackForThisThread();
            if (body) body();
        });
    }

    void ServiceThread::RequestStop() noexcept
    {
        // Release so a body that reads StopRequested() with acquire sees every
        // write made before the stop was requested.
        m_stop.store(true, std::memory_order_release);
        if (m_wake)
        {
            // The callback is expected to lock the consumer's mutex and notify.
            // Swallow rather than propagate: this runs from the destructor.
            try { m_wake(); }
            catch (...) { ARC_ERROR("ServiceThread '{}': wake callback threw", m_debugName); }
        }
    }

    ServiceThread::~ServiceThread()
    {
        RequestStop();
        if (m_thread.joinable())
            m_thread.join();
    }
}
