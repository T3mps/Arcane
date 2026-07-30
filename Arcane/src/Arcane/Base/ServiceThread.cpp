#include <Arcane/Base/ServiceThread.hpp>

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
