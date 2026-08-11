#include <Arcane/Render/GpuBreadcrumbs.hpp>

#include <iterator>
#include <utility>

namespace Arcane
{
    std::uint32_t GpuBreadcrumbs::BeginScope(std::string_view name)
    {
        Entry e;
        e.id = m_nextId++;
        e.name = std::string(name);
        e.depth = static_cast<std::uint32_t>(m_openStack.size());
        if (!m_openStack.empty())
        {
            e.hasParent = true;
            e.parentId = m_openStack.back();
        }
        m_openStack.push_back(e.id);

        const std::uint32_t id = e.id;
        if (m_ring.size() >= kRingCapacity)
            m_ring.erase(m_ring.begin()); // evict oldest
        m_ring.push_back(std::move(e));
        return id;
    }

    void GpuBreadcrumbs::EndScope(std::uint32_t token)
    {
        // Well-nested callers always end LIFO; scan from the back but
        // defensively search the whole stack in case of misuse.
        for (auto it = m_openStack.rbegin(); it != m_openStack.rend(); ++it)
        {
            if (*it == token)
            {
                m_openStack.erase(std::next(it).base());
                break;
            }
        }

        const std::size_t idx = FindIndex(token);
        if (idx < m_ring.size())
            m_ring[idx].closed = true;
    }

    void GpuBreadcrumbs::OnMarkerWritten(std::uint32_t id, bool begin)
    {
        const std::size_t idx = FindIndex(id);
        if (idx >= m_ring.size())
            return;
        if (begin)
            m_ring[idx].beginWritten = true;
        else
            m_ring[idx].endWritten = true;
    }

    GpuBreadcrumbs::Snapshot GpuBreadcrumbs::Capture() const
    {
        Snapshot snap;

        // lastCompleted: the ring is kept in ascending-id order (append at
        // the back, evict from the front), so the highest-id scope whose
        // end marker was observed is the first hit scanning back-to-front.
        for (auto it = m_ring.rbegin(); it != m_ring.rend(); ++it)
        {
            if (it->endWritten)
            {
                snap.lastCompleted = it->name;
                break;
            }
        }

        // inFlight: every entry whose begin marker was observed but not
        // its end (directly in flight), plus -- for each -- its still-open
        // ancestor chain (an ancestor with no EndScope call yet, whether or
        // not it has any marker evidence of its own). A closed or evicted
        // ancestor stops that step of the walk but not the ones above it --
        // an intermediate scope closing says nothing about whether ITS
        // ancestors are still open.
        std::vector<bool> included(m_ring.size(), false);
        for (std::size_t i = 0; i < m_ring.size(); ++i)
        {
            const Entry& e = m_ring[i];
            if (!e.beginWritten || e.endWritten)
                continue;
            included[i] = true;

            bool hasParent = e.hasParent;
            std::uint32_t parentId = e.parentId;
            while (hasParent)
            {
                const std::size_t pIdx = FindIndex(parentId);
                if (pIdx >= m_ring.size())
                    break; // ancestor evicted -- can't see further up

                if (!m_ring[pIdx].closed)
                    included[pIdx] = true;

                hasParent = m_ring[pIdx].hasParent;
                parentId = m_ring[pIdx].parentId;
            }
        }

        for (std::size_t i = 0; i < m_ring.size(); ++i)
            if (included[i])
                snap.inFlight.push_back(m_ring[i].name);

        return snap;
    }

    std::size_t GpuBreadcrumbs::FindIndex(std::uint32_t id) const
    {
        for (std::size_t i = 0; i < m_ring.size(); ++i)
            if (m_ring[i].id == id)
                return i;
        return m_ring.size();
    }
}
