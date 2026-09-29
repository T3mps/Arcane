#pragma once

// InspectorHost (inspector-ownership spec s3): which source's page each
// Inspector INSTANCE shows. Pure state -- no ImGui -- so the rules are unit-
// tested (EditorInspectorHostTest): the last-selecting source wins; focus is
// never an event (there is no focus API here at all); pin holds a keyed page;
// a closed source releases; history depth 32, prune-on-invalidate (PruneStale,
// once per frame, through the sources' PURE Resolves), skip at navigation,
// never persisted; every source but the fallback releases on project switch;
// the fallback is INVALIDATED (history + pins) on every scene swap.

#include "Panels/InspectorSource.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // The app's per-source "was that a selection EVENT" filter (spec s3.1):
    // a selection GESTURE (the source's epoch moved) that left a non-empty
    // key. A clear is never an event; a key that changed without a gesture
    // (a prune re-primaried a multi-selection after a deletion or a
    // structural undo) is never an event; re-selecting the already-selected
    // thing IS one -- it re-asserts the Inspector on that source without
    // moving history (Push's echo compare swallows the duplicate).
    struct SelectionEdge
    {
        std::uint64_t lastEpoch = 0;
        bool Observe(std::uint64_t epoch, std::string_view key)
        {
            const bool event = epoch != lastEpoch && !key.empty();
            lastEpoch = epoch;
            return event;
        }
    };

    class InspectorHost
    {
    public:
        static constexpr std::size_t kHistoryDepth = 32;
        // ids 0..7: a FIXED pool of stable window keys (UE keeps Details 1..4),
        // so a closed slot's imgui.ini dock entry is the one its next opener
        // inherits. Never minted, always the lowest free slot.
        static constexpr int kMaxInstances = 8;

        struct Instance
        {
            int  id = 0;                    // 0 = the permanent "Inspector" window
            bool pinned = false;
            InspectorSource* pinnedSource = nullptr;   // null once the key died or the source closed
            std::string pinnedKey;
            std::string pinnedName;         // survives the source's death (the "closed" note)
            bool sourceClosed = false;      // true only for a CLOSED source; a dead key on a live one reads "Pinned selection is gone"
        };
        struct HistoryEntry
        {
            InspectorSource* source;
            std::string key;
            std::string label;              // InspectorCrumbText at push time, refreshed when the cursor leaves it
        };

        // `fallback` (the scene) is registered for the host's whole life.
        explicit InspectorHost(InspectorSource& fallback);

        void AddSource(InspectorSource& source);
        // Mark pins on it closed, invalidate, erase. No-op for the fallback.
        void RemoveSource(InspectorSource& source);
        // The source's KEYS just died (the scene's registry was replaced by
        // New/Open Scene or a project switch) but the source lives on: prune
        // every history entry naming it, release every pin naming it
        // ("Pinned selection is gone", not "<name> closed"), keep it
        // registered and keep Current() as it is. Works on the fallback --
        // that is its main caller.
        void InvalidateSource(InspectorSource& source);
        // A selection EVENT in `source` (never focus). Ignored for a source
        // that was never added. Makes it current; pushes {source, key, label}
        // unless the key is empty or equals the cursor entry (a navigation echo).
        void NotifySelected(InspectorSource& source);
        [[nodiscard]] InspectorSource& Current() const noexcept { return *m_current; }
        // What instance `id` shows: its pinned source (null while dead/closed) or Current().
        [[nodiscard]] InspectorSource* SourceFor(int instanceId) const;

        [[nodiscard]] const std::vector<Instance>& Instances() const noexcept { return m_instances; }
        [[nodiscard]] Instance* Find(int id);
        int  AddInstance();                              // lowest free id in [1, kMaxInstances); -1 when the pool is full
        void RemoveInstance(int id);                     // id 0 is refused
        void SetInstanceIds(std::span<const int> extras); // ini restore: exactly {0} + the valid, deduplicated ids exist afterwards
        // True when Current() has a page for its current key: the pin is only
        // offered for a resolvable page (UE's details lock exists only while
        // objects are viewed). Non-const: PageFor is.
        [[nodiscard]] bool CanPin();
        void SetPinned(int id, bool pinned);   // pin captures Current() + key + name; REFUSED (no-op) when !CanPin()
        // In-window navigation of a PINNED instance (a breadcrumb click): only
        // that instance's pinnedKey changes -- never Current(), the source's
        // selection or the history. No-op for an unknown, unpinned or closed instance.
        void RepinKey(int id, std::string key);

        [[nodiscard]] bool CanGoBack() const noexcept { return !m_history.empty() && m_cursor > 0; }
        [[nodiscard]] bool CanGoForward() const noexcept { return !m_history.empty() && m_cursor + 1 < m_history.size(); }
        bool GoBack();
        bool GoForward();
        // Land on ONE chosen entry: no walking. On failure that single entry is pruned.
        bool JumpTo(std::size_t index);
        [[nodiscard]] const HistoryEntry* BackEntry() const noexcept { return CanGoBack() ? &m_history[m_cursor - 1] : nullptr; }
        [[nodiscard]] const HistoryEntry* ForwardEntry() const noexcept { return CanGoForward() ? &m_history[m_cursor + 1] : nullptr; }
        [[nodiscard]] const std::vector<HistoryEntry>& History() const noexcept { return m_history; }
        [[nodiscard]] std::size_t HistoryCursor() const noexcept { return m_cursor; }
        // Drop every entry whose source no longer resolves its key; the cursor
        // keeps pointing at the same surviving entry (index arithmetic, never a
        // key search). Once per frame from the draw: <= kHistoryDepth pure lookups.
        void PruneStale();

        // Project switch: every non-fallback source is dropped, the history
        // cleared, every instance unpinned. Instances themselves stay (they
        // are layout, per-ini, not project state).
        void ReleaseAll();

    private:
        [[nodiscard]] bool Registered(const InspectorSource* s) const;
        void Push(InspectorSource& source, std::string key);
        void RefreshCursorLabel();
        // Restore entry `index` in its source: on success the cursor lands
        // there, its source becomes current and its key is re-snapshotted from
        // live state; on failure that single entry is erased (the cursor keeps
        // pointing at the same surviving entry). GoBack/GoForward/JumpTo share it.
        bool TryLand(std::size_t index);
        void EraseHistoryIf(const std::function<bool(const HistoryEntry&)>& pred);

        InspectorSource* m_fallback;
        std::vector<InspectorSource*> m_sources;
        InspectorSource* m_current;
        std::vector<HistoryEntry> m_history;
        std::size_t m_cursor = 0;
        std::vector<Instance> m_instances;
    };
}
