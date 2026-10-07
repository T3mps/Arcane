#include <Panels/AssetActivityLog.hpp>
#include "Settings/AssetBrowserSettings.hpp"   // editor.assets.activityLogCapacity (settings S6-38)

#include <Arcane/Config/Settings.hpp>

#include <algorithm>
#include <utility>

namespace Arcane::Editor
{
    AssetActivityLog::AssetActivityLog()
        : AssetActivityLog(static_cast<std::size_t>(std::max(Arcane::Settings<AssetBrowserSettings>().activityLogCapacity, 1)))
    {
    }

    AssetActivityLog::AssetActivityLog(std::size_t capacity)
        : m_capacity(std::max<std::size_t>(capacity, 1))
    {
    }

    void AssetActivityLog::Push(AssetActivityEntry e)
    {
        // Below capacity: grow. m_next tracks the size in this regime (it
        // is only ever assigned size() % m_capacity), which is exactly what
        // ForEachNewestFirst's walk-backward-from-m_next-1 needs once the
        // ring later fills -- see that function for why the same formula
        // covers both regimes.
        if (m_ring.size() < m_capacity)
        {
            m_ring.push_back(std::move(e));
            m_next = m_ring.size() % m_capacity;
            return;
        }

        // At capacity: overwrite the oldest slot (m_next) and advance.
        m_ring[m_next] = std::move(e);
        m_next = (m_next + 1) % m_capacity;
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

    std::optional<std::string> TombstoneName(const AssetActivityLog& log, const Arcane::Guid& guid)
    {
        std::optional<std::string> o;
        bool done = false;
        log.ForEachNewestFirst([&](const AssetActivityEntry& e)
        {
            if (done || e.guid != guid) return;
            if (e.kind == AssetActivityKind::Deleted) { o = e.name; done = true; }
            else if (e.kind == AssetActivityKind::Created) done = true;
        });
        return o;
    }
}
