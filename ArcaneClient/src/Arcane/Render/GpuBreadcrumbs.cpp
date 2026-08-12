#include <Arcane/Render/GpuBreadcrumbs.hpp>

#include <iterator>
#include <utility>

namespace Arcane
{
    std::uint32_t GpuBreadcrumbs::BeginScope(std::string_view name)
    {
        // Frozen: the token contract holds (monotonic, never reused, so the
        // marker slot `id % capacity` stays well-defined for callers already
        // holding tokens) but the ring and open stack stay exactly as they
        // were when the device was lost. EndScope/OnMarkerWritten already
        // treat an un-ringed token as a safe no-op.
        if (m_frozen.load(std::memory_order_acquire))
            return m_nextId++;

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
        if (m_frozen.load(std::memory_order_acquire))
            return;   // see Freeze(): the open stack is part of the frozen picture

        // Well-nested callers always end LIFO; scan from the back but
        // defensively search the whole stack in case of misuse. This is
        // pure open-stack bookkeeping (for later BeginScope depth/parent
        // computation) -- it deliberately does not touch the ring entry
        // itself. See the header comment: EndScope carries no information
        // Capture() can trust about what the GPU actually reached.
        for (auto it = m_openStack.rbegin(); it != m_openStack.rend(); ++it)
        {
            if (*it == token)
            {
                m_openStack.erase(std::next(it).base());
                break;
            }
        }
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
        // its end (directly in flight), plus -- for each -- its ancestor
        // chain up to the root, EXCEPT any ancestor whose own end marker
        // WAS observed. This is marker-evidence-only: GPU execution order
        // guarantees an enclosing scope's end marker is written after its
        // descendants', so a descendant still in flight means every
        // enclosing scope without a written end marker is still executing
        // -- independent of whether the ancestor has any BEGIN marker
        // evidence of its own, and independent of CPU-side EndScope state
        // (the CPU routinely races ahead and closes a scope long before
        // the GPU reaches, or ever reaches, its end marker). An evicted
        // ancestor stops that step of the walk but not the ones above it.
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

                if (!m_ring[pIdx].endWritten)
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
