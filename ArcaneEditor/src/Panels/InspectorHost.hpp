#pragma once

// InspectorHost (inspector-ownership spec s3): which source's page each
// Inspector INSTANCE shows. Pure state -- no ImGui -- so the rules are unit-
// tested (EditorInspectorHostTest): the last-selecting source wins; focus is
// never an event (there is no focus API here at all); pin holds a keyed page;
// a closed source releases; history depth 32, prune-on-invalidate (PruneStale,
// once per frame, through the sources' PURE Resolves), skip at navigation,
// never persisted; every source but the fallback and a permanent one releases
// on project switch; the fallback is INVALIDATED (history + pins) on every
// scene swap; each instance routes through its own filter (spec 2026-09-29 s3).

#include "Panels/InspectorKinds.hpp"
#include "Panels/InspectorSource.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
        // The default layout's "Assets only" instance ("Inspector 2", spec s6).
        static constexpr int kAssetsInstanceId = 1;

        struct Instance
        {
            int  id = 0;                    // 0 = the permanent "Inspector" window
            bool pinned = false;
            InspectorSource* pinnedSource = nullptr;   // null once the key died or the source closed
            std::string pinnedKey;
            std::string pinnedName;         // survives the source's death (the "closed" note)
            bool sourceClosed = false;      // true only for a CLOSED source; a dead key on a live one reads "Pinned selection is gone"
            InspectorFilter filter;         // per-instance; layout state (persisted as Filters= in [EditorInspector][Instances], spec s7)
        };
        struct HistoryEntry
        {
            InspectorSource* source;
            std::string key;
            std::string label;              // InspectorCrumbText at push time, refreshed when the cursor leaves it
        };

        // `fallback` (the scene) is registered for the host's whole life.
        explicit InspectorHost(InspectorSource& fallback);

        // `permanent`: lives as long as the host, like the fallback (the Asset
        // Browser's source) -- ReleaseAll invalidates it instead of dropping it.
        void AddSource(InspectorSource& source, bool permanent = false);
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
        // What instance `id` shows: its pinned source (null while dead/closed);
        // else Current() when its filter admits it (always, for All); else the
        // admitted source with the latest selection stamp; else the fallback
        // when admitted; else the most recently added admitted source; else null.
        [[nodiscard]] InspectorSource* SourceFor(int instanceId) const;

        [[nodiscard]] const std::vector<Instance>& Instances() const noexcept { return m_instances; }
        [[nodiscard]] Instance* Find(int id);
        [[nodiscard]] const Instance* Find(int id) const;
        // Lowest free id in [1, kMaxInstances); -1 when the pool is full. A
        // slot closed earlier this session comes back with the filter it had
        // (its [Window] entry still docks it where that filter made sense --
        // final review m2); a never-used slot is All.
        int  AddInstance();
        void RemoveInstance(int id);                     // id 0 is refused; remembers the slot's filter for AddInstance
        void SetInstanceIds(std::span<const int> extras); // ini restore: exactly {0} + the valid, deduplicated ids exist afterwards
        // True when the page instance `instanceId` shows is resolvable: the
        // pin is only offered for a resolvable page (UE's details lock exists
        // only while objects are viewed). Non-const: PageFor is.
        [[nodiscard]] bool CanPin(int instanceId);
        void SetPinned(int id, bool pinned);   // pin captures SourceFor(id) + its key + name; REFUSED (no-op) when !CanPin(id)
        bool SetFilter(int id, InspectorFilter filter);   // false: unknown id, or it excludes every catalog kind (refused, unchanged)
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
        // Per-instance navigation over the SHARED history (spec 2026-09-29 s4).
        // An instance's POSITION is anchored on what it SHOWS: the history
        // entry whose (source, key) equals its routed SourceFor(id) and that
        // source's live SelectionKey(), the one nearest the cursor. Only when
        // no entry matches does it fall back to the last entry its filter
        // admits at or before the cursor. Back = the nearest admitted index
        // BEFORE the position; Forward = the nearest admitted index AFTER it.
        // So another instance moving the shared cursor never makes these
        // arrows lie (final review I2/F1). A landing is a selection: it moves
        // the cursor and Current() for every instance. Push's forward
        // truncation stays global (browser semantics; parked, final-fix N).
        [[nodiscard]] std::optional<std::size_t> PositionFor(int id) const;
        [[nodiscard]] std::optional<std::size_t> BackIndex(int id) const;
        [[nodiscard]] std::optional<std::size_t> ForwardIndex(int id) const;
        [[nodiscard]] bool CanGoBack(int id) const { return BackIndex(id).has_value(); }
        [[nodiscard]] bool CanGoForward(int id) const { return ForwardIndex(id).has_value(); }
        bool GoBack(int id);
        bool GoForward(int id);
        [[nodiscard]] const HistoryEntry* BackEntry(int id) const;
        [[nodiscard]] const HistoryEntry* ForwardEntry(int id) const;
        // The history dropdowns: admitted indices before the position (nearest first) / after it.
        [[nodiscard]] std::vector<std::size_t> BackIndices(int id) const;
        [[nodiscard]] std::vector<std::size_t> ForwardIndices(int id) const;
        // Drop every entry whose source no longer resolves its key; the cursor
        // keeps pointing at the same surviving entry (index arithmetic, never a
        // key search). Once per frame from the draw: <= kHistoryDepth pure lookups.
        void PruneStale();

        // Project switch: every source but the fallback and the permanent
        // ones is dropped, the stamps and the history cleared, every instance
        // unpinned. Instances and their filters stay (they are layout,
        // per-ini, not project state).
        void ReleaseAll();

        // Window > Reset Layout / a fresh layout (spec s6/s7): exactly {0, 1};
        // 0 = All but Assets, 1 (kAssetsInstanceId) = Assets only. Pins released.
        void ApplyDefaultInspectorLayout();
        // A pre-feature layout (no Filters= line, spec s6): 0 = All but Assets,
        // and an Assets-only instance at the lowest free id (kept if one already
        // exists -- idempotent). Returns that instance's id (-1 = the pool is
        // full: instance 0's filter still applies).
        int  UpgradeLegacyInspectorLayout();
        // Set by the ini handler's ApplyAllFn when a load saw no Filters= line;
        // the app consumes it once. ReleaseAll never touches it (a windowed
        // project switch loads the incoming ini AFTER releasing).
        [[nodiscard]] bool TakeLegacyLayoutUpgrade() noexcept { return std::exchange(m_legacyLayoutPending, false); }
        // Ini-handler plumbing (InspectorWindows.cpp), not for app code.
        void NoteLayoutReadBegin() noexcept { m_sawFiltersLine = false; }
        void NoteFiltersLine() noexcept { m_sawFiltersLine = true; }
        void NoteLayoutReadEnd() noexcept { if (!m_sawFiltersLine) m_legacyLayoutPending = true; }

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
        void Stamp(InspectorSource& source) { m_stamps[&source] = ++m_stampClock; }

        InspectorSource* m_fallback;
        std::vector<InspectorSource*> m_sources;
        InspectorSource* m_current;
        std::vector<HistoryEntry> m_history;
        std::size_t m_cursor = 0;
        std::vector<Instance> m_instances;
        std::vector<InspectorSource*> m_permanent;                   // AddSource(.., true); the fallback is implicitly permanent
        std::unordered_map<InspectorSource*, std::uint64_t> m_stamps; // last selection event / history landing per source
        std::uint64_t m_stampClock = 0;
        std::unordered_map<int, InspectorFilter> m_closedFilters;   // slot id -> its filter when it was closed (session only)
        bool m_sawFiltersLine = false;       // this ini load saw a Filters= line
        bool m_legacyLayoutPending = false;  // a load without one: TakeLegacyLayoutUpgrade's flag
    };
}
