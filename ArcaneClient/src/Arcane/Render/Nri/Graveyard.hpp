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
//
// Not reentrant: Bury()/Reap()/Drain() must never be called from within a
// destroy thunk that this same Graveyard is currently running via Reap()/
// Drain() (e.g. a compound resource's destroy thunk burying a dependent
// sub-resource on the SAME graveyard). Asserted in debug (a guard flag is
// set for the duration of Reap()/Drain(), checked at the top of all three
// methods). Chosen deliberately over making reentrant Bury() a supported,
// structurally-safe operation: a reentrantly-buried entry's correct
// treatment is ambiguous (does it join the fence value currently being
// reaped, or wait for the next call?) and nothing in this phase needs the
// answer yet. Revisit if a real caller needs it -- Reap()/Drain() already
// execute via re-fetched indices rather than iterators specifically so nothing
// worse than "assert fires in debug" happens if this contract slips through
// in release (a reentrant Bury() alone cannot dangle; it simply defers its
// entry to the graveyard's next Reap()/Drain() call).
//
// Exception safety: if a destroy thunk throws, Reap()/Drain() still erase
// every entry up to and including the throwing one before the exception
// propagates -- a throwing thunk (and everything buried before it) can never
// re-run on a later Reap()/Drain() call. Entries after the throw point are
// left pending, to be attempted on the next call.

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
        // prefix without a per-call sort. NOT reentrant: must not be called
        // from within a destroy thunk this graveyard is currently running
        // (asserted in debug -- see the class comment above).
        void Bury(std::uint64_t fenceValue, Destroyer destroy);

        // Runs every thunk buried with fenceValue <= completedValue, in the
        // order they were buried, then forgets them. Safe to call with
        // completedValue lower than every pending fenceValue (a no-op) or
        // with nothing pending at all. NOT reentrant (asserted in debug --
        // see the class comment above). Exception-safe: a throwing thunk's
        // entry, and every entry before it, is erased before the exception
        // propagates out of this call -- never re-run by a later Reap/Drain.
        void Reap(std::uint64_t completedValue);

        // Runs every remaining thunk regardless of fence value, then forgets
        // them. Teardown / device-loss path ONLY: the caller must already
        // have made the GPU idle (e.g. a blocking device/queue wait) before
        // calling this, or it can free a resource the GPU is still using.
        // NOT reentrant (asserted in debug -- see the class comment above).
        // Exception-safe: same throwing-thunk contract as Reap() above.
        void Drain();

        [[nodiscard]] std::size_t Pending() const noexcept;

    private:
        struct Burial
        {
            std::uint64_t fenceValue;
            Destroyer     destroy;
        };

        // Shared by Reap()/Drain(): executes the first `count` entries of
        // m_graves by index (re-fetched fresh each iteration, never a cached
        // iterator/pointer), extracting each Destroyer out of the vector
        // before invoking it, and erases the executed prefix via a scope
        // guard so a throwing thunk -- or a thunk that reentrantly Buries,
        // growing/reallocating m_graves -- can never corrupt the sweep or
        // cause a later re-run. See Graveyard.cpp for the full reasoning.
        void ExecutePrefix(std::size_t count);

        std::vector<Burial> m_graves;

        // True for the duration of a Reap()/Drain() call (including while a
        // destroy thunk is running) -- guards Bury()/Reap()/Drain() against
        // reentrancy from within a running thunk. See the class comment.
        bool m_executing = false;
    };
}
