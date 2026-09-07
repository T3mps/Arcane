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
// Task 15: AssetKind/AssetKindOf/MatchesFilter/AssetEntry/BuildAssetEntries
// and the rest of the asset-classification family (Slice 6's Asset Browser)
// live directly in this header now -- Panels/AssetBrowser.hpp/.cpp is
// retired. KindIcon/KindLabel just below are promoted from AssetBrowser.cpp's
// file-local (internal-linkage) duplicates -- AssetsPanel.cpp's own copy and
// this file's own RailKindLabel both used to shadow them independently; both
// collapse onto these, the one shared definition from here on.

#include "Widgets/IconsLucide.h"   // KindIcon glyphs

#include <Arcane/Assets/Assets.hpp>             // AssetRef, AssetRefKind
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (surfaceFor, MaterialSurfaceFilterForComponent)
#include <Arcane/Project/AssetRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    enum class AssetKind : int
    {
        Material = 0,
        Texture,
        Audio,
        Font,
        Data,
        Scene,
        Sprite,
        // GPU crash diagnostics arc, Task 10: .arcdiag crash/hang/gpu-crash/
        // gpu-stall reports (Diag::Envelope). Kept right before Other, the
        // same "newest kind slots in ahead of the catch-all" placement
        // Sprite used when it was added (docs/specs/2026-07-28-sprite-
        // asset-design.md:116).
        Diagnostic,
        // F2a, Task 9: .arcmesh procedural mesh assets (MeshDocument). Same
        // placement rule as Diagnostic above -- ahead of the catch-all.
        Mesh,
        Other,
    };
    inline constexpr int kAssetKindCount = 10;

    // The ImGui drag-drop payload type for browser rows (the params panel's
    // texture slots accept it). Payload bytes = AssetDragPayload (POD).
    inline constexpr const char* kAssetDragType = "ARCANE_ASSET";
    struct AssetDragPayload
    {
        Arcane::Guid guid;
        AssetKind kind;
    };

    // Classify by extension (matches the registry's own scan rules: .arcmat and
    // .json are native; the binary list mirrors AssetRegistry's IsImportedBinary).
    inline AssetKind AssetKindOf(std::string_view mountPath)
    {
        const std::size_t dot = mountPath.rfind('.');
        if (dot == std::string_view::npos)
            return AssetKind::Other;
        std::string ext(mountPath.substr(dot));
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".arcmat")
            return AssetKind::Material;
        if (ext == ".arcscene")
            return AssetKind::Scene;
        if (ext == ".arcsprite")
            return AssetKind::Sprite;
        if (ext == ".arcdiag")
            return AssetKind::Diagnostic;
        if (ext == ".arcmesh")
            return AssetKind::Mesh;
        for (const char* e : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" })
            if (ext == e) return AssetKind::Texture;
        for (const char* e : { ".wav", ".ogg", ".mp3", ".flac" })
            if (ext == e) return AssetKind::Audio;
        if (ext == ".ttf" || ext == ".otf")
            return AssetKind::Font;
        if (ext == ".json")
            return AssetKind::Data;
        return AssetKind::Other;
    }

    struct AssetEntry
    {
        Arcane::Guid guid;
        std::string  mountPath;   // "game://materials/glow.arcmat"
        std::string  name;        // "glow" (stem)
        AssetKind    kind = AssetKind::Other;
    };

    // Registry snapshot -> classified entries, sorted by name (path breaks ties)
    // for a stable listing.
    inline std::vector<AssetEntry> BuildAssetEntries(const Arcane::AssetRegistry& registry)
    {
        std::vector<AssetEntry> entries;
        for (auto& [guid, mountPath] : registry.All())
        {
            AssetEntry e;
            e.guid = guid;
            e.kind = AssetKindOf(mountPath);
            const std::size_t slash = mountPath.rfind('/');
            std::string_view file = slash == std::string::npos
                                        ? std::string_view(mountPath)
                                        : std::string_view(mountPath).substr(slash + 1);
            const std::size_t dot = file.rfind('.');
            e.name = std::string(dot == std::string_view::npos ? file : file.substr(0, dot));
            e.mountPath = std::move(mountPath);
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const AssetEntry& a, const AssetEntry& b)
                  { return a.name != b.name ? a.name < b.name : a.mountPath < b.mountPath; });
        return entries;
    }

    // Inspector asset-ref (Guid) fields: infer the expected asset kind from the
    // FIELD NAME -- reflection carries no per-field attributes yet, so this
    // heuristic is the seam until it does. Case-insensitive substring match:
    // "material" -> Material, "texture" -> Texture, "sprite" -> Sprite,
    // "mesh" -> Mesh; anything else -> -1 (all kinds, same convention as
    // MatchesFilter's kindFilter). Sprite is checked AFTER material/texture
    // ON PURPOSE: a field named e.g. "spriteMaterial" contains both
    // substrings and must still resolve Material (the material IS what such
    // a field means), so the material/texture branches have to win the
    // race. `mesh` is checked LAST, after sprite, for the identical reason:
    // `MeshRenderer::materialOverride` faces the same race a hypothetical
    // "meshMaterial" field would (it contains both "mesh" and "material"),
    // and the material branch must win it exactly as spriteMaterial's does
    // -- so `mesh` sits at the end, immediately before the -1 fallback,
    // rather than being checked before material/texture/sprite.
    inline int AssetKindFilterForFieldName(std::string_view fieldName)
    {
        std::string lower(fieldName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("material") != std::string::npos)
            return static_cast<int>(AssetKind::Material);
        if (lower.find("texture") != std::string::npos)
            return static_cast<int>(AssetKind::Texture);
        if (lower.find("sprite") != std::string::npos)
            return static_cast<int>(AssetKind::Sprite);
        if (lower.find("mesh") != std::string::npos)
            return static_cast<int>(AssetKind::Mesh);
        return -1;
    }

    // Owning-component context for a material-ref field: which MaterialSurface
    // must candidates have? -1 = unfiltered. Extends the field-name-heuristic
    // seam just above until reflection carries per-field attributes of its
    // own -- "material" alone (AssetKindFilterForFieldName's answer) never
    // says whether the field feeds a 2D sprite or a 3D mesh; the COMPONENT
    // that owns the field is what answers that, so this reads the owning
    // component's type name instead of the field's.
    //
    // Substring match, not exact equality: the call site hands this
    // Astra::TypeMeta::typeName verbatim (InspectorView.cpp), which is the
    // compiler-derived, NAMESPACE-QUALIFIED name ("Arcane::SpriteRenderer"),
    // not the bare identifier the interface doc quotes -- a substring test
    // matches either shape without asking the caller to strip anything
    // first. No ordering race like AssetKindFilterForFieldName's
    // material/mesh split: "SpriteRenderer" and "MeshRenderer" share no
    // substring, so the two checks below can never both fire for one name.
    // This DOES assume no future component's name embeds either literal as
    // a substring of its OWN name (e.g. a hypothetical "MySpriteRendererFX")
    // -- exactly the same assumption AssetKindFilterForFieldName already
    // makes about field names, just one level up at the component. Revisit
    // this function if that ever stops holding.
    [[nodiscard]] inline int MaterialSurfaceFilterForComponent(std::string_view componentName)
    {
        if (componentName.find("SpriteRenderer") != std::string_view::npos)
            return static_cast<int>(Arcane::MaterialSurface::Sprite);
        if (componentName.find("MeshRenderer") != std::string_view::npos)
            return static_cast<int>(Arcane::MaterialSurface::Mesh);
        return -1;
    }

    // A Guid field whose NAME says it is an IDENTITY, not an asset reference:
    // exactly "id" or "guid", case-insensitive. Same name-heuristic seam as
    // AssetKindFilterForFieldName above (reflection carries no per-field
    // attributes yet). Arcane::Identity.id is the live case -- every entity
    // carries one, the asset registry can never resolve it, and running it
    // through the dangling-reference styling painted "(missing)" on healthy
    // entities. Exact match on purpose: substring would eat "textureId",
    // which the kind heuristic correctly claims as a texture reference.
    inline bool IsIdentityGuidFieldName(std::string_view fieldName)
    {
        std::string lower(fieldName);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower == "id" || lower == "guid";
    }

    // kindFilter: -1 = all kinds. `search`: case-insensitive substring over
    // name AND mount path; empty matches everything.
    inline bool MatchesFilter(const AssetEntry& entry, int kindFilter, std::string_view search)
    {
        if (kindFilter >= 0 && static_cast<int>(entry.kind) != kindFilter)
            return false;
        if (search.empty())
            return true;

        auto containsCI = [](std::string_view hay, std::string_view needle)
        {
            if (needle.size() > hay.size())
                return false;
            auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        };
        return containsCI(entry.name, search) || containsCI(entry.mountPath, search);
    }

    // KindIcon/KindLabel (Task 15 consolidation): the Lucide glyph / display
    // string for a kind, shared by every representation (rail, table rows,
    // peek tooltip, preview pane, drag-source label).
    inline const char* KindIcon(AssetKind kind)
    {
        switch (kind)
        {
            case AssetKind::Material: return ICON_LC_PALETTE;
            case AssetKind::Texture:  return ICON_LC_IMAGE;
            case AssetKind::Audio:    return ICON_LC_MUSIC;
            case AssetKind::Font:     return ICON_LC_TYPE;
            case AssetKind::Data:     return ICON_LC_FILE_JSON;
            case AssetKind::Scene:    return ICON_LC_CLAPPERBOARD;
            // ICON_LC_STICKER exists in IconsLucide.h (grepped; ICON_LC_GHOST is
            // also present but STICKER reads as "sprite" and is preferred by the
            // brief's fallback order).
            case AssetKind::Sprite:   return ICON_LC_STICKER;
            // ICON_LC_BUG exists in IconsLucide.h (grepped: IconsLucide.h:307) --
            // no nearest-match fallback needed.
            case AssetKind::Diagnostic: return ICON_LC_BUG;
            // A 3D box, for a procedural mesh -- ICON_LC_BOX exists in
            // IconsLucide.h (grepped: IconsLucide.h:287).
            case AssetKind::Mesh:     return ICON_LC_BOX;
            case AssetKind::Other:    return ICON_LC_FILE;
        }
        return ICON_LC_FILE;
    }

    inline const char* KindLabel(AssetKind kind)
    {
        switch (kind)
        {
            case AssetKind::Material: return "Material";
            case AssetKind::Texture:  return "Texture";
            case AssetKind::Audio:    return "Audio";
            case AssetKind::Font:     return "Font";
            case AssetKind::Data:     return "Data";
            case AssetKind::Scene:    return "Scene";
            case AssetKind::Sprite:   return "Sprite";
            case AssetKind::Diagnostic: return "Diagnostic";
            case AssetKind::Mesh:     return "Mesh";
            case AssetKind::Other:    return "Other";
        }
        return "Other";
    }

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
        std::string  groupName;   // Type::Group ONLY: the FULL content-directory path
                                   // ("textures/patterns/", "materials/", "Content/") --
                                   // this is the open-state KEY (m_groupOpen, PushID),
                                   // unchanged in meaning by the 2026-09-07 nested-group
                                   // pass. NOT what renders as the label any more.
        std::string  groupLabel;  // Type::Group ONLY: the DISPLAY label -- leaf segment
                                   // only, plus trailing '/' ("patterns/" for
                                   // "textures/patterns/"). Top-level dirs and the
                                   // "Content/" root are their own leaf, so this equals
                                   // groupName unchanged for depth 0.
        int          groupDepth = 0;  // Nesting depth of this row's OWNING group -- ROOT-
                                   // ANCHORED (spec s6, 2026-09-07 2nd revision): only the
                                   // "Content/" root itself is depth 0; every other
                                   // directory is 1 + its nesting below Content/ (a top-
                                   // level dir like "materials/" is depth 1). Set on
                                   // Group rows AND on Asset/Child rows (their owning
                                   // group's depth), so the panel can compute the 20px/
                                   // level indent without re-deriving it from the guid.
        int          groupCount = 0;
        Arcane::Guid guid;        // Asset/Child
    };

    // Nesting depth of a content-directory string ("a/b/c/" style, "Content/" for the
    // synthetic root -- AssetPanelEntry::folder's own doc comment). ROOT-ANCHORED
    // (spec s6, 2026-09-07 second revision): "Content/" is the table's real depth-0
    // root; EVERY other directory is now 1 + its nesting depth below Content/ -- a
    // top-level directory like "materials/" is depth 1 (not 0), "textures/patterns/"
    // is depth 2 (not 1). Every folder string here carries a trailing '/'
    // (MakeBaseEntry's invariant), so for anything but the literal root, depth is
    // just "how many '/' separators" (no longer minus one -- that subtraction was
    // exactly what made a top-level dir depth 0; root-anchoring folds that dir in
    // as Content/'s own child instead).
    inline int GroupDepthOf(std::string_view folder)
    {
        if (folder.empty() || folder == "Content/")
            return 0;
        int slashes = 0;
        for (char c : folder)
            if (c == '/') ++slashes;
        return slashes;
    }

    // Display label for a group row: the LEAF segment only, trailing '/' kept
    // ("textures/patterns/" -> "patterns/"). Top-level dirs and "Content/" ARE their
    // own leaf already, so this returns the input unchanged for depth 0 -- no special-
    // casing needed (spec s6: "top-level groups unchanged").
    inline std::string GroupLabelOf(std::string_view folder)
    {
        if (folder.empty())
            return std::string(folder);
        const std::string_view trimmed = folder.substr(0, folder.size() - 1);   // drop trailing '/'
        const std::size_t slash = trimmed.rfind('/');
        return std::string(slash == std::string_view::npos ? trimmed : trimmed.substr(slash + 1)) + "/";
    }

    // Immediate PARENT content directory of a group folder, or "" if `folder` IS
    // the "Content/" root (the one group with no parent). ROOT-ANCHORED (spec s6,
    // 2026-09-07 second revision): "textures/patterns/" -> "textures/" (unchanged);
    // "materials/" -> **"Content/"** (was "" before root-anchoring -- every
    // top-level directory is now Content/'s own child, not a sibling with no
    // parent). Walking this repeatedly yields the folder's full ancestor chain,
    // root-most last -- "Content/" itself always terminates the walk.
    inline std::string GroupParentOf(std::string_view folder)
    {
        if (folder.empty() || folder == "Content/")
            return {};   // the literal root has no parent
        const std::string_view trimmed = folder.substr(0, folder.size() - 1);
        const std::size_t slash = trimmed.rfind('/');
        if (slash == std::string_view::npos)
            return "Content/";   // a top-level dir's parent is the root group
        return std::string(trimmed.substr(0, slash + 1));
    }

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
