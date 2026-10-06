#include "Panels/InspectorHost.hpp"
#include "Settings/InspectorSettings.hpp"   // editor.inspector.historyDepth / maxInstances (settings S6-37)

#include <Arcane/Config/Settings.hpp>

#include <algorithm>

namespace Arcane::Editor
{
    InspectorHost::InspectorHost(InspectorSource& fallback)
        : m_fallback(&fallback)
        , m_maxInstances(Arcane::Settings<InspectorSettings>().maxInstances)
        , m_sources{ &fallback }
        , m_current(&fallback)
    {
        m_instances.push_back(Instance{});   // id 0
    }

    bool InspectorHost::Registered(const InspectorSource* s) const
    {
        return std::find(m_sources.begin(), m_sources.end(), s) != m_sources.end();
    }

    void InspectorHost::AddSource(InspectorSource& source, bool permanent)
    {
        if (!Registered(&source)) m_sources.push_back(&source);
        if (permanent && std::find(m_permanent.begin(), m_permanent.end(), &source) == m_permanent.end())
            m_permanent.push_back(&source);
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
        m_stamps.erase(&source);
        std::erase(m_permanent, &source);
        if (m_current == &source) m_current = m_fallback;
    }

    void InspectorHost::PruneStale()
    {
        EraseHistoryIf([](const HistoryEntry& e) { return !e.source->Resolves(e.key); });
    }

    void InspectorHost::RefreshLabels()
    {
        for (HistoryEntry& e : m_history)
            if (e.source && Registered(e.source) && e.source->Resolves(e.key)) e.label = InspectorCrumbText(*e.source, e.source->PageFor(e.key));
        for (Instance& i : m_instances)
            if (i.pinned && i.pinnedSource && Registered(i.pinnedSource) && i.pinnedSource->Resolves(i.pinnedKey)) i.pinnedName = i.pinnedSource->SourceName();
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
        const auto depth = static_cast<std::size_t>(Arcane::Settings<InspectorSettings>().historyDepth);
        if (m_history.size() > depth)
            m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - depth));
        m_cursor = m_history.size() - 1;
    }

    void InspectorHost::NotifySelected(InspectorSource& source)
    {
        if (!Registered(&source)) return;
        std::string key = source.SelectionKey();
        if (key.empty()) return;   // a deselect is not a selection event
        m_current = &source;
        Stamp(source);
        Push(source, std::move(key));
    }

    InspectorSource* InspectorHost::SourceFor(int instanceId) const
    {
        const Instance* inst = Find(instanceId);
        if (!inst) return nullptr;
        if (inst->pinned) return inst->pinnedSource;
        const InspectorFilter& f = inst->filter;
        if (f.Admits(m_current->Kind())) return m_current;          // All always lands here
        InspectorSource* best = nullptr;
        std::uint64_t bestStamp = 0;
        for (InspectorSource* s : m_sources)
        {
            if (!f.Admits(s->Kind())) continue;
            const auto it = m_stamps.find(s);
            if (it != m_stamps.end() && it->second > bestStamp) { best = s; bestStamp = it->second; }
        }
        if (best) return best;
        if (f.Admits(m_fallback->Kind())) return m_fallback;
        for (auto it = m_sources.rbegin(); it != m_sources.rend(); ++it)   // most recently added
            if (f.Admits((*it)->Kind())) return *it;
        return nullptr;
    }

    InspectorHost::Instance* InspectorHost::Find(int id)
    {
        for (Instance& inst : m_instances)
            if (inst.id == id) return &inst;
        return nullptr;
    }

    const InspectorHost::Instance* InspectorHost::Find(int id) const
    {
        for (const Instance& inst : m_instances)
            if (inst.id == id) return &inst;
        return nullptr;
    }

    int InspectorHost::AddInstance()
    {
        for (int id = 1; id < m_maxInstances; ++id)
            if (!Find(id))
            {
                Instance inst;
                inst.id = id;
                if (const auto it = m_closedFilters.find(id); it != m_closedFilters.end()) inst.filter = it->second;
                m_instances.push_back(inst);
                return id;
            }
        return -1;
    }

    void InspectorHost::RemoveInstance(int id)
    {
        if (id == 0) return;
        if (const Instance* inst = Find(id)) m_closedFilters[id] = inst->filter;
        std::erase_if(m_instances, [id](const Instance& i) { return i.id == id; });
    }

    void InspectorHost::SetInstanceIds(std::span<const int> extras)
    {
        std::vector<Instance> kept;
        kept.push_back(*Find(0));
        for (const int id : extras)
        {
            if (id < 1 || id >= m_maxInstances) continue;
            if (std::any_of(kept.begin(), kept.end(), [id](const Instance& i) { return i.id == id; })) continue;
            if (const Instance* old = Find(id)) kept.push_back(*old);
            else { Instance inst; inst.id = id; kept.push_back(inst); }
        }
        std::sort(kept.begin(), kept.end(), [](const Instance& a, const Instance& b) { return a.id < b.id; });
        m_instances = std::move(kept);
    }

    void InspectorHost::ApplyDefaultInspectorLayout()
    {
        const int ids[] = { kAssetsInstanceId };
        SetInstanceIds(ids);
        for (Instance& inst : m_instances)
        {
            inst.pinned = false;
            inst.pinnedSource = nullptr;
            inst.pinnedKey.clear();
            inst.pinnedName.clear();
            inst.sourceClosed = false;
        }
        Find(0)->filter = InspectorFilter::AllBut("assets");
        Find(kAssetsInstanceId)->filter = InspectorFilter::Only("assets");
    }

    int InspectorHost::UpgradeLegacyInspectorLayout()
    {
        Find(0)->filter = InspectorFilter::AllBut("assets");
        const InspectorFilter assetsOnly = InspectorFilter::Only("assets");
        for (const Instance& inst : m_instances)
            if (inst.filter == assetsOnly) return inst.id;   // idempotent: an Assets-only instance already exists
        const int id = AddInstance();
        if (id >= 0) Find(id)->filter = assetsOnly;
        return id;
    }

    bool InspectorHost::CanPin(int instanceId)
    {
        InspectorSource* src = SourceFor(instanceId);
        return src && src->PageFor(src->SelectionKey()) != nullptr;
    }

    void InspectorHost::SetPinned(int id, bool pinned)
    {
        Instance* inst = Find(id);
        if (!inst) return;
        if (pinned && !CanPin(id)) return;   // nothing to hold: a pin never holds emptiness
        InspectorSource* src = pinned ? SourceFor(id) : nullptr;   // BEFORE the flag flips: SourceFor reads it
        inst->pinned = pinned;
        inst->sourceClosed = false;
        inst->pinnedSource = src;
        inst->pinnedKey = src ? src->SelectionKey() : std::string{};
        inst->pinnedName = src ? src->SourceName() : std::string{};
    }

    bool InspectorHost::SetFilter(int id, InspectorFilter filter)
    {
        Instance* inst = Find(id);
        if (!inst || filter.ExcludesEveryKind()) return false;
        inst->filter = filter.Sanitized();
        return true;
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
            Stamp(*source);
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

    std::optional<std::size_t> InspectorHost::PositionFor(int id) const
    {
        const Instance* inst = Find(id);
        if (!inst || m_history.empty()) return std::nullopt;
        // Anchor: the entry the instance SHOWS -- its routed source + the key
        // of the page it draws: a PINNED instance's pinnedKey (its
        // pinnedSource is what SourceFor returns), else that source's live
        // key -- nearest the cursor (ties prefer the earlier).
        if (const InspectorSource* src = SourceFor(id))
        {
            const std::string key = inst->pinned ? inst->pinnedKey : src->SelectionKey();
            std::optional<std::size_t> best;
            std::size_t bestDist = 0;
            for (std::size_t i = 0; i < m_history.size(); ++i)
            {
                if (m_history[i].source != src || m_history[i].key != key) continue;
                const std::size_t dist = i > m_cursor ? i - m_cursor : m_cursor - i;
                if (!best || dist < bestDist) { best = i; bestDist = dist; }
            }
            if (best) return best;
        }
        // Fallback: the last admitted entry at or before the cursor.
        for (std::size_t i = m_cursor + 1; i-- > 0;)
            if (inst->filter.Admits(m_history[i].source->Kind())) return i;
        return std::nullopt;
    }

    std::optional<std::size_t> InspectorHost::BackIndex(int id) const
    {
        const Instance* inst = Find(id);
        const auto pos = PositionFor(id);
        if (!inst || !pos) return std::nullopt;
        for (std::size_t i = *pos; i-- > 0;)
            if (inst->filter.Admits(m_history[i].source->Kind())) return i;
        return std::nullopt;
    }

    std::optional<std::size_t> InspectorHost::ForwardIndex(int id) const
    {
        const Instance* inst = Find(id);
        if (!inst) return std::nullopt;
        // No position = nothing admitted at or before the cursor, so scanning
        // from 0 finds the same first admitted entry as scanning past the cursor.
        const auto pos = PositionFor(id);
        for (std::size_t i = pos ? *pos + 1 : 0; i < m_history.size(); ++i)
            if (inst->filter.Admits(m_history[i].source->Kind())) return i;
        return std::nullopt;
    }

    const InspectorHost::HistoryEntry* InspectorHost::BackEntry(int id) const
    {
        const auto i = BackIndex(id);
        return i ? &m_history[*i] : nullptr;
    }

    const InspectorHost::HistoryEntry* InspectorHost::ForwardEntry(int id) const
    {
        const auto i = ForwardIndex(id);
        return i ? &m_history[*i] : nullptr;
    }

    bool InspectorHost::GoBack(int id)
    {
        RefreshCursorLabel();
        while (const auto i = BackIndex(id))
            if (TryLand(*i)) return true;                      // failure erased *i; recompute
        return false;
    }

    bool InspectorHost::GoForward(int id)
    {
        RefreshCursorLabel();
        while (const auto i = ForwardIndex(id))
            if (TryLand(*i)) return true;
        return false;
    }

    std::vector<std::size_t> InspectorHost::BackIndices(int id) const
    {
        std::vector<std::size_t> out;
        const Instance* inst = Find(id);
        const auto first = BackIndex(id);
        if (!inst || !first) return out;
        for (std::size_t i = *first + 1; i-- > 0;)
            if (inst->filter.Admits(m_history[i].source->Kind())) out.push_back(i);
        return out;
    }

    std::vector<std::size_t> InspectorHost::ForwardIndices(int id) const
    {
        std::vector<std::size_t> out;
        const Instance* inst = Find(id);
        const auto first = ForwardIndex(id);
        if (!inst || !first) return out;
        for (std::size_t i = *first; i < m_history.size(); ++i)
            if (inst->filter.Admits(m_history[i].source->Kind())) out.push_back(i);
        return out;
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
        for (InspectorSource* p : m_permanent) m_sources.push_back(p);   // the asset source survives a project switch
        m_stamps.clear();
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
