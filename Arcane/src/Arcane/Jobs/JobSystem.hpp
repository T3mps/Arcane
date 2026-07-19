#pragma once

// Jobs: the engine owns ONE enkiTS TaskScheduler per process and exposes it to
// Astra through the IWorkScheduler seam. Astra creates no threads; this adapter
// is the only thread source for the simulation. Lives in Arcane.dll so the
// scheduler is a single shared instance; hosts receive a shared_ptr to inject
// into Registry::Config and Astra::ParallelExecutor.

#include <Arcane/Base/Api.hpp>

#include <Astra/Core/WorkScheduler.hpp>

#include <cstdint>
#include <memory>

namespace Arcane
{
    struct ITaskExecutor;   // <Arcane/Jobs/TaskExecutor.hpp>; full def used in the .cpp

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr<Impl> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API JobSystem
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

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
