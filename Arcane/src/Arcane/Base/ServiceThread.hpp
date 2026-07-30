#pragma once

// ServiceThread: one dedicated thread for LONG-LIVED BLOCKING work -- shader
// compiles, msbuild waits, file IO. It owns thread lifetime and a stop flag,
// and NOTHING else.
//
// THIS IS NOT THE FORK-JOIN POOL. Compute parallelism (ECS iteration, physics)
// goes to JobSystem (enkiTS), whose workers must never block -- JobSystem.hpp
// calls it "the only thread source for the simulation", and a blocked worker
// starves it. Never submit blocking work there. Two mechanisms, one rule.
//
// Deliberately NOT a work queue. ShaderCompiler's std::deque<Job> under its own
// mutex/cv IS its debounce-and-coalesce machinery; a generic FIFO would destroy
// it. So `main` is the consumer's entire loop body and it owns whatever queue it
// needs; `wake` exists so the destructor can unblock a body sleeping on the
// consumer's own condition variable.

#include <Arcane/Base/Api.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API ServiceThread
    {
    public:
        // `main` runs once, on the new thread; a service loops inside it until
        // StopRequested(). `wake` is invoked by RequestStop BEFORE the join --
        // it must take the same lock the body waits under, then notify.
        ServiceThread(std::string debugName,
                      std::function<void()> main,
                      std::function<void()> wake = {});

        // Always stops and joins. Never detaches.
        ~ServiceThread();

        ServiceThread(const ServiceThread&)            = delete;
        ServiceThread& operator=(const ServiceThread&) = delete;
        ServiceThread(ServiceThread&&)                 = delete;
        ServiceThread& operator=(ServiceThread&&)      = delete;

        // Idempotent. Sets the flag, then calls `wake`.
        void RequestStop() noexcept;

        [[nodiscard]] bool StopRequested() const noexcept { return m_stop.load(std::memory_order_acquire); }
        [[nodiscard]] const std::string& DebugName() const noexcept { return m_debugName; }

    private:
        std::string           m_debugName;
        std::function<void()> m_wake;
        std::atomic<bool>     m_stop{false};
        std::thread           m_thread;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
