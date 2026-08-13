#include "Graveyard.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    void Graveyard::Bury(std::uint64_t fenceValue, Destroyer destroy)
    {
        // Reentrancy contract (see the class comment in Graveyard.hpp):
        // Bury() must not be called from within a destroy thunk that this
        // graveyard's own Reap()/Drain() is currently running -- m_executing
        // is true for exactly that window.
        ARC_ASSERT(!m_executing,
                    "Graveyard::Bury: reentrant call from within this graveyard's own "
                    "Reap()/Drain() destroy thunk is forbidden");

        // Nondecreasing-fenceValue invariant: Reap()'s front-partition
        // assumes the due prefix is contiguous, which only holds if burials
        // arrive in the order their fence values complete in -- exactly what
        // a single queue timeline produces. A caller burying out of order
        // would silently corrupt Reap()'s prefix assumption (a thunk could
        // be skipped past, or run before its fence value has completed), so
        // this is asserted rather than merely documented.
        ARC_ASSERT(m_graves.empty() || fenceValue >= m_graves.back().fenceValue,
                    "Graveyard::Bury: fenceValue must be nondecreasing across burials");

        m_graves.push_back(Burial{ fenceValue, std::move(destroy) });
    }

    void Graveyard::Reap(std::uint64_t completedValue)
    {
        ARC_ASSERT(!m_executing,
                    "Graveyard::Reap: reentrant call from within this graveyard's own "
                    "Reap()/Drain() destroy thunk is forbidden");
        m_executing = true;
        struct ExecutingGuard
        {
            bool& flag;
            ~ExecutingGuard() noexcept { flag = false; }
        } executingGuard{m_executing};

        // m_graves is sorted by fenceValue (Bury enforces nondecreasing
        // order), so every due thunk (fenceValue <= completedValue) sits in
        // one contiguous prefix. std::stable_partition finds that prefix and
        // preserves burial order within it in a single linear pass -- no
        // reordering actually occurs since the range is already sorted, but
        // this is the documented, plan-mandated shape of the split. This is
        // pure comparison work over `fenceValue` only (never touches a
        // Destroyer or runs a thunk), so it cannot trigger reentrancy.
        const auto firstNotDue = std::stable_partition(
            m_graves.begin(), m_graves.end(),
            [completedValue](const Burial& burial) { return burial.fenceValue <= completedValue; });

        ExecutePrefix(static_cast<std::size_t>(firstNotDue - m_graves.begin()));
    }

    void Graveyard::Drain()
    {
        ARC_ASSERT(!m_executing,
                    "Graveyard::Drain: reentrant call from within this graveyard's own "
                    "Reap()/Drain() destroy thunk is forbidden");
        m_executing = true;
        struct ExecutingGuard
        {
            bool& flag;
            ~ExecutingGuard() noexcept { flag = false; }
        } executingGuard{m_executing};

        ExecutePrefix(m_graves.size());
    }

    void Graveyard::ExecutePrefix(std::size_t count)
    {
        // executedCount tracks how many FRONT entries have been (about to
        // be, or already) run -- incremented BEFORE each thunk is invoked,
        // not after, so a throwing thunk still counts as "executed": its
        // GPU-side effects are unknown/partial once Destroy* has been
        // called, so it must never be retried. EraseGuard erases
        // [begin, begin + executedCount) unconditionally on scope exit,
        // including on exception unwind, so the executed prefix (throwing
        // entry included) is gone from m_graves before the exception (if
        // any) propagates out of Reap()/Drain() -- it can never re-run on a
        // later call. Entries at/after `count` (not due) and any entries
        // past the throw point (due, but never attempted) are left pending.
        std::size_t executedCount = 0;
        struct EraseGuard
        {
            Graveyard&    self;
            std::size_t&  executedCount;
            ~EraseGuard()
            {
                self.m_graves.erase(self.m_graves.begin(),
                                     self.m_graves.begin() + static_cast<std::ptrdiff_t>(executedCount));
            }
        } eraseGuard{*this, executedCount};

        while (executedCount < count)
        {
            // Index, not iterator/pointer: re-fetching m_graves[index] fresh
            // every iteration means a reentrant Bury() growing (and possibly
            // reallocating) m_graves cannot leave anything dangling here --
            // it is forbidden by contract (asserted in Bury()), but this
            // loop does not rely on that assert to stay memory-safe.
            const std::size_t index = executedCount;
            ++executedCount;

            // Extract the Destroyer OUT of the vector before invoking it:
            // the callable's own execution must not be sitting inside
            // storage that its own side effects (a reentrant Bury()'s
            // push_back) could reallocate out from under it.
            Destroyer destroy = std::move(m_graves[index].destroy);
            if (destroy)
                destroy();
        }
    }

    std::size_t Graveyard::Pending() const noexcept
    {
        return m_graves.size();
    }

    Graveyard::~Graveyard()
    {
#if defined(ARCANE_DEBUG)
        // Fatal in debug: a nonempty graveyard at destruction means the
        // owner never Reaped up to the final fence value nor called Drain()
        // -- pending destroy thunks are about to be silently dropped (leaking
        // the GPU-side resources they were guarding) rather than executed.
        ARC_ASSERT(m_graves.empty(),
                    "Graveyard destroyed with pending burials -- Reap() to the final "
                    "completed fence value or call Drain() before it goes out of scope");
#else
        // Release: never crash a shipped build over a housekeeping omission.
        // Drain the stragglers instead, loudly, since running destroy thunks
        // from a destructor with no fence-completion guarantee is exactly
        // the class of bug this type exists to prevent -- it just isn't
        // fatal enough to abort a running game over.
        if (!m_graves.empty())
        {
            ARC_WARN("[nri] Graveyard destroyed with {} pending burial(s) -- draining now "
                      "from the destructor (no fence-completion guarantee for this drain)",
                      m_graves.size());
        }
        Drain();
#endif
    }
}
