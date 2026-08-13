#include "Graveyard.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    void Graveyard::Bury(std::uint64_t fenceValue, Destroyer destroy)
    {
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
        // m_graves is sorted by fenceValue (Bury enforces nondecreasing
        // order), so every due thunk (fenceValue <= completedValue) sits in
        // one contiguous prefix. std::stable_partition finds that prefix and
        // preserves burial order within it in a single linear pass -- no
        // reordering actually occurs since the range is already sorted, but
        // this is the documented, plan-mandated shape of the split.
        const auto firstNotDue = std::stable_partition(
            m_graves.begin(), m_graves.end(),
            [completedValue](const Burial& burial) { return burial.fenceValue <= completedValue; });

        for (auto it = m_graves.begin(); it != firstNotDue; ++it)
        {
            if (it->destroy)
                it->destroy();
        }

        m_graves.erase(m_graves.begin(), firstNotDue);
    }

    void Graveyard::Drain()
    {
        for (Burial& burial : m_graves)
        {
            if (burial.destroy)
                burial.destroy();
        }

        m_graves.clear();
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
