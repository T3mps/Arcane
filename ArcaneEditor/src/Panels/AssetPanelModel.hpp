#pragma once

// AssetPanelModel (asset-manager redesign, Plan 1 Task 4): the pure, cached,
// headless-testable state behind the Assets panel's Browse lens. Rebuilds are
// driven by explicit dirty marks (MarkDirty/MarkAllDirty), never per-frame --
// see RebuildIfDirty's own doc comment for the cost contract. This unit NEVER
// calls the engine facade directly: everything about one asset (material
// subkind, outgoing refs, cook state) arrives through the injected
// AssetPanelProviders, so the whole TU compiles and runs with ZERO ImGui and
// ZERO Arcane::Assets/Arcane::Project dependency -- the [editor] test drives
// it against a REAL scanned AssetRegistry with fake provider lambdas.
//
// AssetKind/AssetKindOf/MatchesFilter/AssetEntry currently live in
// Panels/AssetBrowser.hpp and are reused here as-is (included below); Task 15
// migrates them into this header once AssetBrowser.* is retired -- do not
// move them ahead of that task.

#include "Panels/AssetBrowser.hpp"   // AssetKind, AssetKindOf, MatchesFilter, AssetEntry

#include <Arcane/Assets/Assets.hpp>             // AssetRef, AssetRefKind
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane { class AssetRegistry; }

namespace Arcane::Editor
{
    // An asset's cook-pipeline status, as the panel shows it (row markers +
    // the digest bar). Textures/sprites are the only kinds with a real cook
    // pipeline today; everything else defaults to Cooked (CookStateOf, Task 5)
    // unless a permanent refusal diagnostic exists for it.
    enum class CookState : std::uint8_t { Cooked, Queued, Refused, Unknown };

    // The pure cook-state mapping (Task 5): permanentDiag (a permanent cook-
    // diagnostic row exists for the guid -- a refusal) always wins, regardless
    // of kind or pending. Otherwise only Texture/Sprite have a real cook
    // pipeline of their own -- pending is meaningless for every other kind,
    // which reports Cooked unconditionally (an unrecognized kind with no
    // diagnostic also defaults to Cooked, never Unknown -- Unknown is
    // reserved for a guid the model has no provider answer for at all, see
    // AssetPanelEntry::cook's own default).
    [[nodiscard]] CookState CookStateOf(AssetKind kind, bool permanentDiag, bool pending);

    // Per-guid facade queries the model needs, injected by the host (EditorApp,
    // Task 5) so this unit never touches Arcane::Assets/Arcane::Project
    // directly. A left-empty (default-constructed std::function) callable is
    // "no answer" -- the model degrades gracefully (nullopt surface, no refs,
    // CookState::Unknown).
    struct AssetPanelProviders
    {
        std::function<std::optional<Arcane::MaterialSurface>(const Arcane::Guid&)> surfaceFor;
        std::function<std::optional<std::vector<Arcane::AssetRef>>(const Arcane::Guid&)> refsFor;
        std::function<CookState(const Arcane::Guid&)> cookStateFor;
    };

    // One registry entry's panel-facing view: classification, folder grouping,
    // fold state, and cook status -- rebuilt only when its guid is dirtied
    // (see RebuildIfDirty).
    struct AssetPanelEntry
    {
        Arcane::Guid guid;
        std::string  name;        // stem
        std::string  fileName;    // stem + extension (rows show this)
        std::string  mountPath;
        std::string  folder;      // "materials/", nested "fx/glow/", root = "Content/"
        AssetKind    kind = AssetKind::Other;
        std::optional<Arcane::MaterialSurface> surface;  // materials only
        bool         isInstance = false;
        CookState    cook = CookState::Unknown;
        Arcane::Guid foldedUnder;                 // valid => render only as a child
        std::vector<Arcane::Guid> derivedChildren; // 1:1 sprites folded under me

        // Controller ruling (Task 4): true for a sprite that is NOT folded but
        // still names a texture through a `References`-kind ref -- a sliced
        // sub-rect sprite (Assets::ListAssetReferences's own doc comment: a
        // fold only ever happens on a single `DerivesFrom`). Task 10 renders
        // the "sliced" pill from this.
        bool         sliced = false;
    };

    struct AssetPanelRow
    {
        enum class Type : std::uint8_t { Group, Asset, Child };
        Type type = Type::Asset;
        std::string  groupName;   // Type::Group
        int          groupCount = 0;
        Arcane::Guid guid;        // Asset/Child
    };

    struct RailEntry { int kind = -1; std::string label; int count = 0; }; // kind -1 = All

    struct HealthCounts { int total = 0, cooked = 0, queued = 0, refused = 0; };

    // Cached, foldable model over an AssetRegistry snapshot: entries rebuild
    // lazily per dirtied guid, rows/rail rebuild lazily when entries or
    // filters/group-state change. Never touches ImGui or the engine facade.
    class AssetPanelModel
    {
    public:
        void MarkDirty(const Arcane::Guid& id);
        void MarkAllDirty();

        // Rebuilds entries for dirty guids (all, when all-dirty) and the row
        // list when entries OR filters/group-state changed. Cheap (no
        // registry walk, no provider calls) when clean. Returns true if
        // anything rebuilt. registry == nullptr clears the model.
        bool RebuildIfDirty(const Arcane::AssetRegistry* registry,
                            const AssetPanelProviders& p);

        void SetSearch(std::string_view s);       // MatchesFilter semantics
        void SetKindFilter(int kindOrMinus1);
        void SetGroupOpen(const std::string& folder, bool open);
        void SetChildrenOpen(const Arcane::Guid& texture, bool open);

        [[nodiscard]] const std::vector<AssetPanelRow>& Rows() const { return m_rows; }
        [[nodiscard]] const std::vector<RailEntry>&     Rail() const { return m_rail; }
        [[nodiscard]] HealthCounts                       Health() const;
        [[nodiscard]] const AssetPanelEntry*             Find(const Arcane::Guid& id) const;
        // EVERY entry, UNFILTERED -- deliberately distinct from Rows(), which
        // is what the search box and the rail's kind filter left visible.
        // Task 12's create dialog is the first consumer and needs exactly
        // this: its Location combo enumerates the folders that EXIST (not the
        // ones a search happens to be showing) and its parent picker offers
        // every material in the project (a parent reference is a Guid, so a
        // material the Browse lens is currently filtering out is still a
        // perfectly valid parent). Iteration order is unspecified (an
        // unordered_map) -- a consumer that displays these MUST sort.
        [[nodiscard]] const std::unordered_map<Arcane::Guid, AssetPanelEntry>& Entries() const
        { return m_entries; }
        [[nodiscard]] int  ShownAssetCount() const { return m_shownAssetCount; }  // "X of N shown"
        [[nodiscard]] bool Filtered() const;             // search or kind filter active

        Arcane::Guid   selected;                          // THE shared selection
        std::uint32_t  selectionStamp = 0;                // bump on every change
        void Select(const Arcane::Guid& g) { if (g != selected) { selected = g; ++selectionStamp; } }
        void ResetForProjectSwitch();                     // clears everything incl. selected

    private:
        void RebuildRows();
        [[nodiscard]] bool MatchesEntryFilter(const AssetPanelEntry& e) const;

        std::unordered_map<Arcane::Guid, AssetPanelEntry> m_entries;
        std::unordered_set<Arcane::Guid> m_dirty;
        bool m_allDirty = true;   // fresh model: the first RebuildIfDirty does a full build
        bool m_rowsDirty = true;

        std::string m_search;
        int m_kindFilter = -1;   // -1 = all

        // Absent == default. Folder groups default OPEN (spec s5); derived-
        // children groups default COLLAPSED (this struct's own field doc).
        std::unordered_map<std::string, bool> m_groupOpen;
        std::unordered_map<Arcane::Guid, bool> m_childrenOpen;

        std::vector<AssetPanelRow> m_rows;
        std::vector<RailEntry>     m_rail;
        int m_shownAssetCount = 0;
    };
}
