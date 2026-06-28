#pragma once

// ITaskExecutor: the engine's synchronous data-parallel seam (presentation-free,
// Astra-free, no global state -> safe in Core). ParallelFor is the ONLY primitive
// this milestone ships. The enkiTS-backed impl lives in Arcane.dll (JobSystem);
// SerialTaskExecutor below is the deterministic reference + the default when no
// executor is injected. The colored physics solver (Phase D) is the consumer.

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    struct ITaskExecutor
    {
        // Partition [0,count) into DISJOINT sub-ranges (grain >= minBatch where
        // possible) covering it exactly once; invoke fn(begin,end,worker) on each.
        // worker in [0,WorkerCount()) names the running thread (per-worker scratch).
        // BLOCKS until all sub-ranges complete. count==0 is a no-op. Re-entrant:
        // legal to call from within an fn already running on this executor.
        // A minBatch of 0 is treated as 1.
        virtual void ParallelFor(std::size_t count, std::size_t minBatch,
                                 FunctionRef<void(std::size_t begin, std::size_t end,
                                                  std::uint32_t worker)> fn) = 0;

        // Inclusive of the calling thread; always >= 1. Batch-size denominator.
        virtual std::uint32_t WorkerCount() const noexcept = 0;

        virtual ~ITaskExecutor() = default;
    };

    // Single-threaded reference: runs the whole range inline as worker 0.
    class SerialTaskExecutor final : public ITaskExecutor
    {
    public:
        void ParallelFor(std::size_t count, std::size_t /*minBatch*/,
                         FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
        {
            if (count == 0) return;
            fn(0, count, 0);
        }

        std::uint32_t WorkerCount() const noexcept override { return 1; }
    };
}
