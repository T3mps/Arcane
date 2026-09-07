#include <Panels/AssetActivityLog.hpp>

#include <utility>

namespace Arcane::Editor
{
    void AssetActivityLog::Push(AssetActivityEntry e)
    {
        // Below capacity: grow. m_next tracks the size in this regime (it
        // is only ever assigned size() % kCapacity), which is exactly what
        // ForEachNewestFirst's walk-backward-from-m_next-1 needs once the
        // ring later fills -- see that function for why the same formula
        // covers both regimes.
        if (m_ring.size() < kCapacity)
        {
            m_ring.push_back(std::move(e));
            m_next = m_ring.size() % kCapacity;
            return;
        }

        // At capacity: overwrite the oldest slot (m_next) and advance.
        m_ring[m_next] = std::move(e);
        m_next = (m_next + 1) % kCapacity;
    }

    void AssetActivityLog::ForEachNewestFirst(
        const std::function<void(const AssetActivityEntry&)>& fn) const
    {
        const std::size_t n = m_ring.size();
        if (n == 0)
            return;

        // The most recently written slot is always m_next-1 (mod n): below
        // capacity m_next equals the current size (Push's growth branch),
        // so m_next-1 is the index Push just wrote; at capacity m_next is
        // the NEXT slot to overwrite, so m_next-1 is the one most recently
        // written. Walking i = 0..n-1 backward from there yields exactly
        // newest-first over whatever is currently held, wrap included.
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t idx = (m_next + n - 1 - i) % n;
            fn(m_ring[idx]);
        }
    }

    std::size_t AssetActivityLog::Size() const
    {
        return m_ring.size();
    }

    void AssetActivityLog::Clear()
    {
        m_ring.clear();
        m_next = 0;
    }
}
