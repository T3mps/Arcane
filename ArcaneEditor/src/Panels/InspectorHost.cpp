#include "Panels/InspectorHost.hpp"

#include <algorithm>

namespace Arcane::Editor
{
    InspectorHost::InspectorHost(InspectorSource& fallback)
        : m_fallback(&fallback), m_sources{ &fallback }, m_current(&fallback)
    {
        m_instances.push_back(Instance{});   // id 0
    }

    bool InspectorHost::Registered(const InspectorSource* s) const
    {
        return std::find(m_sources.begin(), m_sources.end(), s) != m_sources.end();
    }

    void InspectorHost::AddSource(InspectorSource& source)
    {
        if (!Registered(&source)) m_sources.push_back(&source);
    }

    void InspectorHost::EraseHistoryIf(const std::function<bool(const HistoryEntry&)>& pred)
    {
        // UE FHistoryManager::RemoveHistoryData: for every removed index at or
        // before the cursor the cursor steps back one, so it keeps pointing at
        // the same surviving entry (or the previous one when its own went).
        for (std::size_t i = 0; i < m_history.size();)
        {
            if (!pred(m_history[i])) { ++i; continue; }
            m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(i));
            if (m_cursor >= i && m_cursor > 0) --m_cursor;
        }
        if (m_history.empty()) m_cursor = 0;
        else m_cursor = std::min(m_cursor, m_history.size() - 1);
    }

    void InspectorHost::InvalidateSource(InspectorSource& source)
    {
        EraseHistoryIf([&](const HistoryEntry& e) { return e.source == &source; });
        for (Instance& inst : m_instances)
            if (inst.pinned && inst.pinnedSource == &source)
                inst.pinnedSource = nullptr;      // sourceClosed untouched: "Pinned selection is gone"
    }

    void InspectorHost::RemoveSource(InspectorSource& source)
    {
        if (&source == m_fallback) return;
        for (Instance& inst : m_instances)         // mark BEFORE Invalidate nulls the pointer
            if (inst.pinned && inst.pinnedSource == &source)
                inst.sourceClosed = true;
        InvalidateSource(source);
        std::erase(m_sources, &source);
        if (m_current == &source) m_current = m_fallback;
    }

    void InspectorHost::PruneStale()
    {
        EraseHistoryIf([](const HistoryEntry& e) { return !e.source->Resolves(e.key); });
    }

    void InspectorHost::Push(InspectorSource& source, std::string key)
    {
        if (!m_history.empty())
        {
            const HistoryEntry& at = m_history[m_cursor];
            // Navigation echo: the cursor entry IS (source, key) -- it is re-
            // snapshotted on every restore, so the next-frame re-report matches.
            if (at.source == &source && at.key == key) return;
            m_history.resize(m_cursor + 1);                        // drop forward entries
        }
        m_history.push_back(HistoryEntry{ &source, std::move(key), InspectorCrumbText(source, source.Page()) });
        if (m_history.size() > kHistoryDepth)
            m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - kHistoryDepth));
        m_cursor = m_history.size() - 1;
    }

    void InspectorHost::NotifySelected(InspectorSource& source)
    {
        if (!Registered(&source)) return;
        std::string key = source.SelectionKey();
        if (key.empty()) return;   // a deselect is not a selection event
        m_current = &source;
        Push(source, std::move(key));
    }

    InspectorSource* InspectorHost::SourceFor(int instanceId) const
    {
        for (const Instance& inst : m_instances)
            if (inst.id == instanceId)
                return inst.pinned ? inst.pinnedSource : m_current;
        return nullptr;
    }

    InspectorHost::Instance* InspectorHost::Find(int id)
    {
        for (Instance& inst : m_instances)
            if (inst.id == id) return &inst;
        return nullptr;
    }

    int InspectorHost::AddInstance()
    {
        for (int id = 1; id < kMaxInstances; ++id)
            if (!Find(id)) { Instance inst; inst.id = id; m_instances.push_back(inst); return id; }
        return -1;
    }

    void InspectorHost::RemoveInstance(int id)
    {
        if (id == 0) return;
        std::erase_if(m_instances, [id](const Instance& i) { return i.id == id; });
    }

    void InspectorHost::SetInstanceIds(std::span<const int> extras)
    {
        std::erase_if(m_instances, [](const Instance& i) { return i.id != 0; });
        for (const int id : extras)
            if (id >= 1 && id < kMaxInstances && !Find(id)) { Instance inst; inst.id = id; m_instances.push_back(inst); }
        std::sort(m_instances.begin(), m_instances.end(), [](const Instance& a, const Instance& b) { return a.id < b.id; });
    }

    bool InspectorHost::CanPin()
    {
        return m_current->PageFor(m_current->SelectionKey()) != nullptr;
    }

    void InspectorHost::SetPinned(int id, bool pinned)
    {
        Instance* inst = Find(id);
        if (!inst) return;
        if (pinned && !CanPin()) return;   // nothing to hold: a pin never holds emptiness
        inst->pinned = pinned;
        inst->sourceClosed = false;
        inst->pinnedSource = pinned ? m_current : nullptr;
        inst->pinnedKey = pinned ? m_current->SelectionKey() : std::string{};
        inst->pinnedName = pinned ? m_current->SourceName() : std::string{};
    }

    void InspectorHost::RepinKey(int id, std::string key)
    {
        Instance* inst = Find(id);
        if (!inst || !inst->pinned || inst->sourceClosed) return;
        inst->pinnedKey = std::move(key);
    }

    void InspectorHost::RefreshCursorLabel()
    {
        // UE refreshes the entry it is LEAVING before navigating, so a rename
        // made after the selection does not leave a stale label behind.
        if (m_history.empty()) return;
        HistoryEntry& at = m_history[m_cursor];
        if (at.source == m_current) at.label = InspectorCrumbText(*at.source, at.source->Page());
    }

    bool InspectorHost::TryLand(std::size_t index)
    {
        // Copy the key: RestoreSelection takes a view, and the entry it names
        // is erased below on failure.
        InspectorSource* const source = m_history[index].source;
        const std::string key = m_history[index].key;
        if (source->RestoreSelection(key))
        {
            m_cursor = index;
            m_current = source;
            m_history[index].key = source->SelectionKey();   // re-snapshot from live state: members may be gone, the source may normalize
            return true;
        }
        // Stale: prune only this entry (the same-frame safety net under
        // PruneStale); an entry before the cursor shifts the cursor with it.
        m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < m_cursor) --m_cursor;
        return false;
    }

    bool InspectorHost::GoBack()
    {
        RefreshCursorLabel();
        while (CanGoBack())
            if (TryLand(m_cursor - 1)) return true;
        return false;
    }

    bool InspectorHost::GoForward()
    {
        RefreshCursorLabel();
        while (CanGoForward())
            if (TryLand(m_cursor + 1)) return true;
        return false;
    }

    bool InspectorHost::JumpTo(std::size_t index)
    {
        if (index >= m_history.size() || index == m_cursor) return false;
        RefreshCursorLabel();
        return TryLand(index);
    }

    void InspectorHost::ReleaseAll()
    {
        m_sources.assign(1, m_fallback);
        m_current = m_fallback;
        m_history.clear();
        m_cursor = 0;
        for (Instance& inst : m_instances)
        {
            inst.pinned = false;
            inst.pinnedSource = nullptr;
            inst.pinnedKey.clear();
            inst.pinnedName.clear();
            inst.sourceClosed = false;
        }
    }
}
