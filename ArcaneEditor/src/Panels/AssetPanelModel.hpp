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
// file-local (internal-linkage) duplicates -- the Assets panel's own copy and
// this file's own RailKindLabel both used to shadow them independently; both
// collapse onto these, the one shared definition from here on.

#include "Panels/AssetReferenceIndex.hpp"   // the inverted-reference index behind `unused`
#include "Widgets/IconsLucide.h"            // KindIcon glyphs

#include <Arcane/Assets/Assets.hpp>             // AssetRef, AssetRefKind
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (surfaceFor, MaterialSurfaceFilterForComponent)
#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Serialization/IdentityFieldRule.hpp>   // the shared identity-field rule

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
        // F2c s4.1, Task 9: .gltf/.glb IMPORTED SOURCES. Distinct from Mesh for the same
        // reason Texture is distinct from Sprite -- Model is the imported original, Mesh
        // is the authored .arcmesh that derives from it (and folds under it in the
        // browser). Same placement rule Diagnostic and Mesh used: ahead of the catch-all.
        Model,
        // Source/ in the Asset Browser: C/C++ source files (.cpp/.hpp/.h/...),
        // registered under source:// by AssetRegistry's path-derived-guid rule.
        // Not content -- no cook pipeline (CookStateOf's default), never
        // `unused` (IsUnusedEligible), no document type yet (double-click is
        // the open-in-IDE step's job, not this one's). Same ahead-of-the-
        // catch-all placement every kind since Diagnostic has used.
        Source,
        Other,
    };
    inline constexpr int kAssetKindCount = 12;

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
        // F2c s4.1: the imported originals -- AssetRegistry's IsImportedBinary
        // recognizes the identical pair as sidecar-bearing binaries.
        if (ext == ".gltf" || ext == ".glb")
            return AssetKind::Model;
        for (const char* e : { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" })
            if (ext == e) return AssetKind::Texture;
        for (const char* e : { ".wav", ".ogg", ".mp3", ".flac" })
            if (ext == e) return AssetKind::Audio;
        if (ext == ".ttf" || ext == ".otf")
            return AssetKind::Font;
        if (ext == ".json")
            return AssetKind::Data;
        // Mirrors AssetRegistry.cpp's IsSourceFile list exactly -- the registry
        // decides what registers, this only names what it registered.
        for (const char* e : { ".cpp", ".hpp", ".h", ".c", ".inl", ".cc", ".cxx", ".hxx" })
            if (ext == e) return AssetKind::Source;
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
    //
    // F2c s4.1, Task 9: Model gets NO branch here, deliberately. No component
    // field names a Model today -- MeshRenderer::mesh names an .arcmesh, i.e.
    // AssetKind::Mesh above, not the imported .gltf/.glb it derives from -- so
    // inventing a "model"/"gltf" substring rule would be a guess with no call
    // site to justify it. Add one only when a real field needs it.
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
    //
    // The RULE moved to the engine (Serialization/IdentityFieldRule.hpp) when
    // the scene serializer's v4 manifest collector became its third consumer;
    // this stays as the editor-side spelling the panels already call, and
    // delegates, so there is exactly one definition to change.
    inline bool IsIdentityGuidFieldName(std::string_view fieldName)
    {
        return Arcane::IsIdentityGuidFieldName(fieldName);
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
            // F2c s4.1, Task 9: ICON_LC_BOXES exists in IconsLucide.h (grepped:
            // ArcaneEditor/src/Widgets/IconsLucide.h:289) -- a stack of boxes for the
            // imported ORIGINAL, deliberately distinct from Mesh's single ICON_LC_BOX
            // (the authored asset derived from it) so the two never read as one kind
            // at a glance in the rail.
            case AssetKind::Model:    return ICON_LC_BOXES;
            // ICON_LC_FILE_CODE exists in IconsLucide.h (grepped: IconsLucide.h:708)
            // -- a file with code brackets, distinct from Other's plain ICON_LC_FILE.
            case AssetKind::Source:   return ICON_LC_FILE_CODE;
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
            case AssetKind::Model:    return "Model";
            case AssetKind::Source:   return "Source";
            case AssetKind::Other:    return "Other";
        }
        return "Other";
    }

    // Graph-node accent packed 0xRRGGBB. The five rows asset-manager spec §11.3
    // pins, plus Model which F2c Plan 2 Task 8 adds -- that value EXTENDS the
    // table rather than quoting it. 0 means "no row": the graph panel falls
    // back to Theme::kGrab (#9a9a9a).
    [[nodiscard]] inline std::uint32_t KindAccentRgb(AssetKind kind) noexcept
    {
        switch (kind)
        {
            case AssetKind::Texture:  return 0xb06a5bu;
            case AssetKind::Material: return 0x6a9b5bu;
            case AssetKind::Mesh:     return 0x5b9bb0u;
            case AssetKind::Sprite:   return 0x9b5bb0u;
            case AssetKind::Scene:    return 0xb09b5bu;
            // F2c Plan 2 Task 8: extends §11.3. Adjacent to Mesh's #5b9bb0 in
            // the same muted family (a Model and its Mesh are kin). Distinct
            // from Sprite's #9b5bb0.
            case AssetKind::Model:    return 0x5b7fb0u;
            default:                  return 0;
        }
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

    // Which kinds may ever be flagged `unused` (spec s9.1, VERBATIM as of its
    // original list; F2c s4.1 Task 9 adds Model to it): Texture, Material,
    // Sprite, Mesh and Model -- the kinds whose consumers the reference
    // index fully sees. Scenes are roots (never unused); Data/Audio/Font are
    // consumed by game code no index observes, so they are EXEMPT rather
    // than falsely accused; Diagnostic/Other likewise. Sits beside
    // CookStateOf on purpose -- both are the model's small, pure per-kind
    // policy rules, and both are unit-testable without a model.
    [[nodiscard]] bool IsUnusedEligible(AssetKind kind);

    // Which kinds route through MaterialPreviewHarvester for a REAL rendered
    // thumbnail rather than falling back to their kind icon (F2c Plan 2 Task
    // 10, spec s8): exactly Material and Mesh -- both are "the thing with
    // geometry/slots and a resolved appearance" the harvester knows how to
    // render. Texture is NOT eligible here -- it resolves its OWN artifact
    // thumbnail directly (EditorApp's resolveAssetThumb texture branch), never
    // through the harvester, so routing it through this predicate too would
    // be a second, competing answer for the same guid. Model is deliberately
    // excluded as well: it has no material assignment of its own -- its
    // appearance IS its companion .arcmesh's, and harvesting both would spend
    // two device idles on two near-identical pictures (the ordinary shape has
    // the companion right beneath it, wearing the picture). Everything else
    // has nothing to render a picture of. Sits beside CookStateOf/
    // IsUnusedEligible on purpose -- the model's third small, pure per-kind
    // policy rule, unit-testable without a model or a harvester.
    [[nodiscard]] bool ThumbnailEligible(AssetKind kind);

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
        std::string  folder;      // the group KEY -- MOUNT-ROOTED (spec s5/s6, 2026-09-07
                                   // third revision): UNQUALIFIED for the "game" scheme
                                   // ("materials/", nested "fx/glow/", root = "Content/",
                                   // unchanged since root-anchoring); QUALIFIED for any
                                   // other scheme, "<scheme>://" (that mount's own root,
                                   // rootless) or "<scheme>://<relpath>/" (nested), e.g.
                                   // "diag://" or "diag://crashes/". See GroupDepthOf's
                                   // own header comment for why the two shapes can never
                                   // collide.
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

        // Plan 2 Task 4 (spec s9.1): an ELIGIBLE-kind asset (IsUnusedEligible)
        // that nothing in the project points at -- zero inbound edges in the
        // model's AssetReferenceIndex, counting BOTH edge kinds (References
        // AND DerivesFrom). Recomputed over EVERY entry on any rebuild that
        // touched entries, never only over the dirtied ones: re-pointing one
        // asset changes the inbound count of two OTHERS (its old target and
        // its new one), neither of which is itself dirty. An exempt kind is
        // false here regardless of its inbound count.
        bool         unused = false;
    };

    struct AssetPanelRow
    {
        enum class Type : std::uint8_t { Group, Asset, Child };
        Type type = Type::Asset;
        std::string  groupName;   // Type::Group ONLY: the FULL group KEY -- this is the
                                   // open-state identity (m_groupOpen, PushID), NOT what
                                   // renders as the label. MOUNT-ROOTED (spec s5/s6,
                                   // 2026-09-07 third revision): two shapes, matching
                                   // AssetPanelEntry::folder's own doc comment exactly
                                   // (this field is just that same string, carried onto
                                   // the row) -- UNQUALIFIED for "game" ("textures/
                                   // patterns/", "materials/", "Content/", unchanged since
                                   // root-anchoring), QUALIFIED for any other scheme
                                   // ("diag://", "diag://crashes/", ...). The qualification
                                   // is what keeps a real "game://diagnostics/" directory's
                                   // key from ever colliding with "diag://"'s own root key.
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

    // MOUNT-ROOTED (spec s5/s6, 2026-09-07 third revision, user-directed): one
    // depth-0 root group per POPULATED mount scheme, not just "game://"'s
    // "Content/". `AssetPanelEntry::folder` (and therefore every group KEY --
    // `AssetPanelRow::groupName`, the `m_groupOpen`/`PushID` identity) now comes
    // in two shapes:
    //   - UNQUALIFIED (the "game" scheme -- the project's primary mount, and the
    //     ONE shape that predates mount-rooting): "Content/" (the root, rootless
    //     files) or a bare relative dir path ("materials/", "textures/patterns/"),
    //     EXACTLY as the 2nd (root-anchored) revision left it -- zero format
    //     change for the default mount, zero blast radius on every game-only
    //     fixture that predates this revision.
    //   - QUALIFIED (every OTHER scheme, e.g. "diag"): "<scheme>://" is that
    //     mount's OWN root (rootless), "<scheme>://<relpath>/" is nested --
    //     the identical bare-relative-path convention the unqualified shape
    //     already uses, just carrying an explicit scheme prefix.
    // This asymmetry is the fix for the KEYING HAZARD the spec calls out
    // explicitly: a REAL "game://diagnostics/" directory derives the plain
    // UNQUALIFIED key "diagnostics/" (today's game-only convention, untouched),
    // which can never collide with "diag://"'s own qualified root key or any of
    // its "diag://…/" descendants -- "://" can never appear inside a real path
    // SEGMENT (it is always a scheme boundary in a mount path, and directory
    // names are built purely from path segments), so the qualified and
    // unqualified key spaces are disjoint by construction, independent of
    // whatever a user happens to name a real directory. Labels stay clean
    // either way (GroupLabelOf below strips the qualification entirely before
    // rendering) -- only the KEY, never what's shown, carries it.
    inline std::string MountRootLabel(std::string_view scheme)
    {
        if (scheme == "diag")
            return "Diagnostics/";
        if (scheme == "source")
            return "Source/";   // the on-disk directory's own name, a peer of "Content/"
        return std::string(scheme) + "/";   // unknown/future scheme -> scheme-named root
    }

    // Which mount roots default COLLAPSED rather than the usual default-open
    // (spec s5, third revision): today only "diag://"'s own root key --
    // crash-report noise starts folded away so it never competes with
    // Content/ for attention. Every other group -- every OTHER mount root
    // included -- defaults open, unchanged. Shared verbatim by the model's own
    // open/closed resolution (AssetPanelModel.cpp's GroupOpenOrDefault) and the
    // panel's chevron-glyph default (AssetBrowserPanel.cpp's GroupIsOpen) so the two
    // can never drift apart the way a stale/differing default once did for a
    // different reason (review fix round 1, Important 2's own precedent).
    inline bool GroupDefaultOpen(std::string_view groupKey)
    {
        return groupKey != "diag://";
    }

    // Peer order of the QUALIFIED mount roots (user-directed 2026-09-12):
    // Content/ (unqualified, always first -- GroupKeyLess) then Source/, then
    // any other scheme (plugin content), then diagnostics/ LAST -- crash
    // noise sits at the bottom, the project's own code right under its
    // content. Lower sorts first. Consulted by AssetPanelModel.cpp's
    // GroupKeyLess for the qualified bucket only; the order WITHIN one
    // mount's own subtree stays plain lexicographic (the same `a < b` it
    // always was), because every key under one scheme shares one rank.
    inline int MountSchemeRank(std::string_view scheme)
    {
        if (scheme == "source") return 0;
        if (scheme == "diag")   return 2;
        return 1;
    }

    // Nesting depth of a group KEY (see the shape doc comment just above).
    // Qualified keys measure depth WITHIN their own mount (their scheme's root
    // is depth 0, exactly like "Content/" is for the unqualified/game shape);
    // unqualified keys are unchanged from the root-anchored (2nd) revision.
    // Every folder string carries a trailing '/' except a qualified root
    // ("<scheme>://", nothing after it) and the empty string.
    inline int GroupDepthOf(std::string_view folder)
    {
        if (folder.empty())
            return 0;
        if (const std::size_t sep = folder.find("://"); sep != std::string_view::npos)
        {
            const std::string_view rest = folder.substr(sep + 3);
            if (rest.empty())
                return 0;   // this mount's own root
            int slashes = 0;
            for (char c : rest)
                if (c == '/') ++slashes;
            return slashes;   // same "1 + nesting below the root" rule as unqualified
        }
        if (folder == "Content/")
            return 0;
        int slashes = 0;
        for (char c : folder)
            if (c == '/') ++slashes;
        return slashes;
    }

    // Display label for a group row: the LEAF segment only, trailing '/' kept
    // ("textures/patterns/" -> "patterns/"; a qualified root -> its mount's own
    // label via MountRootLabel, e.g. "diag://" -> "Diagnostics/"). A mount
    // root -- "Content/" or any other -- is its own leaf already, so it never
    // shortens to nothing (spec s6).
    inline std::string GroupLabelOf(std::string_view folder)
    {
        if (folder.empty())
            return std::string(folder);
        if (const std::size_t sep = folder.find("://"); sep != std::string_view::npos)
        {
            const std::string scheme(folder.substr(0, sep));
            const std::string_view rest = folder.substr(sep + 3);
            if (rest.empty())
                return MountRootLabel(scheme);
            const std::string_view trimmed = rest.substr(0, rest.size() - 1);
            const std::size_t slash = trimmed.rfind('/');
            return std::string(slash == std::string_view::npos ? trimmed : trimmed.substr(slash + 1)) + "/";
        }
        const std::string_view trimmed = folder.substr(0, folder.size() - 1);   // drop trailing '/'
        const std::size_t slash = trimmed.rfind('/');
        return std::string(slash == std::string_view::npos ? trimmed : trimmed.substr(slash + 1)) + "/";
    }

    // Immediate PARENT of a group KEY, or "" if `folder` IS a mount's own root
    // (the one group with no parent, per mount). Mount roots are PEERS, never
    // ancestors of each other (spec s6: "closing one never touches another's
    // rows") -- each qualified/unqualified tree terminates at its own root
    // independently. Walking this repeatedly yields the folder's full
    // ancestor chain within its OWN mount, root-most last.
    inline std::string GroupParentOf(std::string_view folder)
    {
        if (folder.empty())
            return {};
        if (const std::size_t sep = folder.find("://"); sep != std::string_view::npos)
        {
            const std::string scheme(folder.substr(0, sep));
            const std::string_view rest = folder.substr(sep + 3);
            if (rest.empty())
                return {};   // this mount's own root has no parent
            const std::string_view trimmed = rest.substr(0, rest.size() - 1);
            const std::size_t slash = trimmed.rfind('/');
            if (slash == std::string_view::npos)
                return scheme + "://";   // a top-level dir under this mount -> its own root
            return scheme + "://" + std::string(trimmed.substr(0, slash + 1));
        }
        if (folder == "Content/")
            return {};   // the literal root has no parent
        const std::string_view trimmed = folder.substr(0, folder.size() - 1);
        const std::size_t slash = trimmed.rfind('/');
        if (slash == std::string_view::npos)
            return "Content/";   // a top-level dir's parent is the root group
        return std::string(trimmed.substr(0, slash + 1));
    }

    struct RailEntry { int kind = -1; std::string label; int count = 0; }; // kind -1 = All

    // `unused` (Plan 2 Task 4) is the count of entries carrying
    // AssetPanelEntry::unused -- the bottom-bar digest's third segment and the
    // Status lens's "unreferenced" stat tile. Folded children count, exactly
    // like they do for `total`.
    struct HealthCounts { int total = 0, cooked = 0, queued = 0, refused = 0, unused = 0; };

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

        // The live reference topology behind `unused` (Plan 2 Task 4), fed by
        // RebuildIfDirty from the very same refsFor answers each rebuilt entry
        // already fetches -- never a second parse. Read-only to consumers: the
        // Status lens reads DanglingTargets()/InboundCount(), the preview pane
        // reads Find()->outbound. Note Find() returns a pointer that is stable
        // across UNRELATED updates but invalidated if that node is
        // tombstone-GC'd -- never cache one across a rebuild pass.
        [[nodiscard]] const AssetReferenceIndex& RefIndex() const { return m_refIndex; }

        // Every entry flagged `unused`, sorted by entry NAME (mount path
        // breaks ties, so the order is fully deterministic despite m_entries
        // being unordered) -- the stable UI order Task 8's Unreferenced card
        // renders verbatim. Same convention Entries()' own doc comment asks
        // of its consumers, applied here once so they need not repeat it.
        [[nodiscard]] std::vector<Arcane::Guid> UnusedGuids() const;

        Arcane::Guid   selected;                          // THE shared selection
        std::uint32_t  selectionStamp = 0;                // bump on every change

        // Plan 3 Task 3: bumped every time the ENTRIES map or the reference
        // index behind it actually changed content -- never for a mere
        // row/filter rebuild (a search keystroke rebuilds Rows() and nothing
        // else, and the Graph lens does not read Rows()). This is the cheap
        // dirty trigger the Graph lens compares against so it rebuilds its
        // own projection (AssetGraphViewModel::Build over Entries() +
        // RefIndex()) exactly when its inputs moved, rather than per frame.
        //
        // MONOTONIC, and deliberately NOT reset by ResetForProjectSwitch (in
        // contrast to selectionStamp, whose only consumer re-seeds itself the
        // same frame): a counter that never goes backwards can never compare
        // equal to a stale "already built at" value a consumer is still
        // holding from the previous project.
        std::uint32_t  entriesStamp = 0;
        void Select(const Arcane::Guid& g) { if (g != selected) { selected = g; ++selectionStamp; } }
        void ResetForProjectSwitch();                     // clears everything incl. selected

    private:
        void RebuildRows();
        [[nodiscard]] bool MatchesEntryFilter(const AssetPanelEntry& e) const;

        std::unordered_map<Arcane::Guid, AssetPanelEntry> m_entries;
        // Kept in lockstep with m_entries by RebuildIfDirty: every rebuilt
        // guid is Update()d with its own (already-fetched) refs answer, every
        // pruned guid with exists=false. Never queried by RebuildRows -- rows
        // are pure entry data; this only feeds AssetPanelEntry::unused and the
        // RefIndex() readers above.
        AssetReferenceIndex m_refIndex;
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
