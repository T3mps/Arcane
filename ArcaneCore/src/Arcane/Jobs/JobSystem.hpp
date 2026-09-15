#pragma once

// Jobs: the engine owns ONE enkiTS TaskScheduler per process and exposes it to
// Astra through the IWorkScheduler seam. Astra creates no threads; this adapter
// is the only thread source for the simulation. Lives in Arcane.dll so the
// scheduler is a single shared instance; hosts receive a shared_ptr to inject
// into Registry::Config and Astra::ParallelExecutor.

#include <Arcane/Core/Api.hpp>

#include <Astra/Core/WorkScheduler.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace Arcane
{
    struct ITaskExecutor;   // <Arcane/Jobs/TaskExecutor.hpp>; full def used in the .cpp

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr<Impl> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_CORE_API JobSystem
    {
    public:
        // threads == 0 -> enkiTS hardware default (GetNumHardwareThreads()).
        explicit JobSystem(uint32_t threads = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        // The shared enkiTS-backed scheduler. Inject the SAME pointer into every
        // module / registry / executor that needs parallelism.
        std::shared_ptr<Mosaic::IWorkScheduler> WorkScheduler() const;

        // The SAME enki pool as WorkScheduler(), exposed through the engine's
        // ITaskExecutor seam (worker-index-aware ParallelFor; FunctionRef callback).
        // Borrowed pointer; lifetime == this JobSystem. Consume from PhysicsWorld etc.
        ITaskExecutor* TaskExecutor() const noexcept;

        uint32_t WorkerCount() const noexcept;

        // F2b Task 12: fire-and-forget CPU work off the calling thread -- the
        // editor's background texture cook is the first production consumer
        // (a watcher-triggered CookSession::CookProject pass must never block
        // the frame). Submitted work runs on ONE of the enkiTS worker threads
        // this JobSystem owns (never the calling thread, unlike WaitforTask's
        // participate-while-waiting shape) and is NOT awaitable by design --
        // a caller that needs a result posts it back through its own queue
        // (see Arcane::Editor::CookQueue for the production shape: a mutex-
        // guarded result list drained once per frame on the main thread).
        //
        // DESTRUCTION DRAINS: ~JobSystem calls enki::TaskScheduler::
        // WaitforAllAndShutdown, which blocks until every submitted task set
        // -- including every fn Submit() ever handed to enkiTS, whether it
        // has started running yet or not -- has completed, before joining the
        // worker threads. A caller does not need to drain explicitly before
        // destroying the JobSystem.
        //
        // enkiTS-pinned-task/TaskSet inside the pimpl: implemented as a
        // heap-allocated enki::TaskSet (setSize 1, so it runs as a single
        // unit on one worker) kept alive in Impl::submitted until it
        // completes; see JobSystem.cpp for the reap-on-next-Submit bookkeeping
        // that keeps that list from growing unbounded across a long session.
        void Submit(std::function<void()> fn);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
