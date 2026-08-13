#pragma once

// NRI substrate (Phase 1, Task 5): fence-tagged deferred destruction.
//
// Contract precondition this exists to satisfy: NRI has no refcounts and no
// deferred-destroy of its own -- destroying a resource the GPU still reads
// is a device fault. Graveyard is the engine-side precondition that makes it
// safe to call an NRI Destroy* function-table entry from ordinary game/
// engine code: Bury() records a destroy thunk against the fence value that
// must COMPLETE before it may run; Reap(completed) runs every thunk with
// fenceValue <= completed, in burial order; Drain() runs everything
// unconditionally (the teardown / device-loss path -- the caller must have
// already made the GPU idle before calling it).
//
// Not thread-safe by design: one Graveyard per queue timeline, owned by the
// single thread that records/submits against that queue and polls its fence.

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Arcane
{
    class ARCANE_API Graveyard
    {
    public:
        using Destroyer = std::function<void()>;

        Graveyard() = default;

        // ~Graveyard(): a graveyard destroyed with pending burials means the
        // caller never Reaped/Drained it -- in debug this ASSERTS the
        // graveyard is empty (fatal: see Graveyard.cpp). A release build
        // instead Drains it (with a WARN), since crashing a shipped build
        // over a housekeeping omission is worse than running the destroy
        // thunks late and un-fenced.
        ~Graveyard();

        // One graveyard per queue timeline, owned in place by its recording
        // thread -- copying would duplicate destroy thunks (each of which
        // must run exactly once).
        Graveyard(const Graveyard&)            = delete;
        Graveyard& operator=(const Graveyard&) = delete;

        // Records `destroy` against `fenceValue`. Callers MUST bury in
        // nondecreasing fenceValue order -- the order a single queue
        // timeline's submissions naturally produce. Reap() relies on this
        // invariant (asserted in debug; see Graveyard.cpp) to find the due
        // prefix without a per-call sort.
        void Bury(std::uint64_t fenceValue, Destroyer destroy);

        // Runs every thunk buried with fenceValue <= completedValue, in the
        // order they were buried, then forgets them. Safe to call with
        // completedValue lower than every pending fenceValue (a no-op) or
        // with nothing pending at all.
        void Reap(std::uint64_t completedValue);

        // Runs every remaining thunk regardless of fence value, then forgets
        // them. Teardown / device-loss path ONLY: the caller must already
        // have made the GPU idle (e.g. a blocking device/queue wait) before
        // calling this, or it can free a resource the GPU is still using.
        void Drain();

        [[nodiscard]] std::size_t Pending() const noexcept;

    private:
        struct Burial
        {
            std::uint64_t fenceValue;
            Destroyer     destroy;
        };

        std::vector<Burial> m_graves;
    };
}
