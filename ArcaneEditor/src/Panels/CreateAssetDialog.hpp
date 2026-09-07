#pragma once

// CreateAssetDialog (asset-manager redesign, Plan 1 Task 12): the ONE create
// flow. Spec s7's invariant, restated here because it is the whole point of
// this unit: **no creation path may bypass `CreateAssetRequest`**. Every
// producer -- the Assets menu, the panel's `+ Create` popup, the rail's `+`,
// a row's `Create` submenu, the preview pane's `New Instance...`, and (Plan 3)
// a graph pin-drag -- is a thin raiser of that request; `EditorApp::
// BeginCreateAsset` is the ONE entry that opens this dialog and
// `EditorApp::ConsumeCreateResult` the ONE dispatcher that mints from it.
//
// TWO HALVES, DELIBERATELY SPLIT ACROSS THE HEADER/TU LINE:
//   * this HEADER is ImGui-FREE and carries the PURE half (the request/result
//     value types, the per-kind vocabulary, and `ValidateCreateName`), so the
//     [editor] test can drive the validation rules headlessly -- the test exe
//     compiles NO ImGui TU (premake5.lua's ArcaneTests file list, which
//     source-compiles only the pure editor units), so the validation MUST be
//     reachable without one. Same header-inline pattern AssetBrowser.hpp's own
//     pure helpers already use.
//   * the .cpp is the ImGui modal, and is compiled into ArcaneEditor ONLY.
//
// Task 13 extends `CreateAssetKind`'s fields (Sprite's texture picker + its
// mint-or-reuse notice, Scene's "set as boot" checkbox) and their dispatch;
// this task ships Material + MaterialInstance end-to-end and opens the shared
// Name+Location anatomy with a disabled Create for the other three (see
// DrawCreateAssetDialog's own comment).

#include "Panels/AssetBrowser.hpp"   // AssetKind (the producer-side reconciliation below)

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>   // MaterialSurface (the Material kind combo)

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;

    // WHAT can be created. NOT AssetKind (Panels/AssetBrowser.hpp): that enum
    // classifies what a registry entry IS (ten values, including kinds nothing
    // can mint -- Texture, Audio, Font, Data, Diagnostic, Other), while this
    // one enumerates the five things the Create menu OFFERS, and splits
    // Material into the two shapes that need different dialog fields
    // (a fresh material vs. an instance OF one). The two numberings are
    // therefore unrelated -- see CreateKindForAssetKind below, which is the
    // ONLY sanctioned bridge between them.
    enum class CreateAssetKind : std::uint8_t
    { Material, MaterialInstance, Mesh, Sprite, Scene };

    inline constexpr int kCreateAssetKindCount = 5;

    // A request to create something. Raised by producers, consumed by
    // EditorApp::BeginCreateAsset. `prefillParent` pre-fills the kind's one
    // asset-valued field (MaterialInstance -> the parent material; Sprite ->
    // the source texture, Task 13); `prefillSurface` pre-picks the Material
    // surface combo (a MaterialSurface value; -1 = none, let the dialog
    // default).
    struct CreateAssetRequest
    {
        CreateAssetKind kind = CreateAssetKind::Material;
        Arcane::Guid    prefillParent;   // instance parent / sprite texture
        int             prefillSurface = -1; // pre-picked MaterialSurface, -1 none
    };

    // ---- per-kind vocabulary (pure; shared by the dialog and its dispatcher)

    [[nodiscard]] inline const char* CreateKindTitle(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:         return "Create Material";
            case CreateAssetKind::MaterialInstance: return "Create Material Instance";
            case CreateAssetKind::Mesh:             return "Create Mesh";
            case CreateAssetKind::Sprite:           return "Create Sprite";
            case CreateAssetKind::Scene:            return "Create Scene";
        }
        return "Create Asset";
    }

    // The file extension the kind mints. Materials and material INSTANCES are
    // both ".arcmat" -- an instance is a material file with a `parent`
    // (MaterialAssetData), not a distinct format.
    [[nodiscard]] inline const char* CreateKindExtension(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:
            case CreateAssetKind::MaterialInstance: return ".arcmat";
            case CreateAssetKind::Mesh:             return ".arcmesh";
            case CreateAssetKind::Sprite:           return ".arcsprite";
            case CreateAssetKind::Scene:            return ".arcscene";
        }
        return "";
    }

    // The Location combo's per-kind default (spec s7: "Location (combo of
    // existing content directories, per-kind default)"), relative to the
    // project's Content/. Empty string = Content/ itself.
    [[nodiscard]] inline const char* CreateKindDefaultFolder(CreateAssetKind k)
    {
        switch (k)
        {
            case CreateAssetKind::Material:
            case CreateAssetKind::MaterialInstance: return "materials/";
            case CreateAssetKind::Mesh:             return "meshes/";
            case CreateAssetKind::Sprite:           return "sprites/";
            case CreateAssetKind::Scene:            return "scenes/";
        }
        return "";
    }

    // The noun the uniqueness message names ("a <noun> named X already exists
    // here"). Derived from the EXTENSION rather than the kind because
    // ValidateCreateName is deliberately kind-agnostic: it validates a name
    // against a directory and an extension, which is all a filesystem
    // uniqueness question needs, and that keeps the function reusable for a
    // kind this enum does not carry yet.
    [[nodiscard]] inline const char* CreateNounForExtension(std::string_view extension)
    {
        if (extension == ".arcmat")    return "material";
        if (extension == ".arcmesh")   return "mesh";
        if (extension == ".arcsprite") return "sprite";
        if (extension == ".arcscene")  return "scene";
        return "file";
    }

    // ---- the Material "Kind" combo (spec s7: sprite / mesh / post) --------
    // The COMBO's own order, which is NOT MaterialSurface's numbering
    // (Fullscreen=0, Sprite=1, Mesh=2 -- MaterialSource.hpp:71): the mock's
    // menu hint reads "sprite / mesh / post", and `Fullscreen` DISPLAYS as
    // "post" everywhere in this panel (spec's global constraint). Two
    // orderings, so two explicit mappings rather than a cast -- the same
    // discipline CreateKindForAssetKind above applies to the other enum pair
    // this task had to reconcile.
    inline constexpr const char* kMaterialSurfaceLabels[] = { "sprite", "mesh", "post" };
    inline constexpr int kMaterialSurfaceCount = 3;

    [[nodiscard]] inline int MaterialSurfaceComboIndex(Arcane::MaterialSurface s)
    {
        switch (s)
        {
            case Arcane::MaterialSurface::Sprite:     return 0;
            case Arcane::MaterialSurface::Mesh:       return 1;
            case Arcane::MaterialSurface::Fullscreen: return 2;
        }
        return 2;
    }

    [[nodiscard]] inline Arcane::MaterialSurface MaterialSurfaceForComboIndex(int index)
    {
        switch (index)
        {
            case 0:  return Arcane::MaterialSurface::Sprite;
            case 1:  return Arcane::MaterialSurface::Mesh;
            default: return Arcane::MaterialSurface::Fullscreen;
        }
    }

    // The dialog's default when nothing pre-picked a surface: "post"
    // (Fullscreen). Chosen to PRESERVE what the retired `Assets -> Create ->
    // Material...` menu item minted (CreateMaterialAt's own Fullscreen
    // default) -- collapsing two menu items into one dialog must not silently
    // change what the surviving one produces.
    inline constexpr int kMaterialSurfaceDefaultIndex = 2;

    // The subkind pill text for a material's surface -- the SAME three strings
    // the combo offers, so the picker's pills and the combo can never drift.
    [[nodiscard]] inline const char* MaterialSurfacePillText(Arcane::MaterialSurface s)
    { return kMaterialSurfaceLabels[MaterialSurfaceComboIndex(s)]; }

    // ---- the producer-side AssetKind -> CreateAssetKind bridge -------------
    // The rail's per-kind `+` and a row's `Create` submenu both start from an
    // AssetKind (what the rail row / the clicked row IS) and must raise a
    // CreateAssetKind (what the dialog creates). THE TWO ENUMS DO NOT SHARE A
    // NUMBERING -- AssetKind::Sprite is 6 and CreateAssetKind::Sprite is 3 --
    // so the bridge is spelled ONCE, here, and applied at the PRODUCER: the
    // `requestCreateKind` field's contract stays "a CreateAssetKind value",
    // with no tagged-source branch at the consumer to keep in lockstep.
    // nullopt for the six AssetKinds nothing can mint (Texture/Audio/Font/
    // Data/Diagnostic/Other) -- matching AssetsPanel.cpp's own
    // RailKindCreatable gate, which is what keeps those rails from offering a
    // `+` in the first place.
    [[nodiscard]] inline std::optional<CreateAssetKind> CreateKindForAssetKind(AssetKind k)
    {
        switch (k)
        {
            case AssetKind::Material: return CreateAssetKind::Material;
            case AssetKind::Mesh:     return CreateAssetKind::Mesh;
            case AssetKind::Sprite:   return CreateAssetKind::Sprite;
            case AssetKind::Scene:    return CreateAssetKind::Scene;
            default:                  return std::nullopt;
        }
    }

    // ---- name validation (PURE; the [editor] test's whole surface) ---------
    struct CreateNameCheck { bool ok = false; std::string message; };

    // Validate a proposed asset name against the directory it would land in.
    // Rules run in UE's deliberate cheap->expensive order (its own
    // AssetViewUtils.cpp:1420-1509 precedent): syntax first (a pure string
    // scan), then length (arithmetic), then uniqueness LAST -- the only rule
    // that touches the filesystem, so an obviously-bad name never pays for a
    // stat. Each rule's message is DISTINCT: the character message and the
    // "already exists" message must never be confusable, or the fix a user
    // has to make stops being obvious from the text.
    //
    // `extension` is the kind's extension WITH its dot (".arcmat"); it counts
    // toward the length budget and completes the file name the uniqueness
    // check looks for. `targetDir` may be nonexistent (a folder the create
    // will make) -- `exists()` on a path under it simply answers false, which
    // is the correct answer for uniqueness.
    // The length budget, in characters of the composed absolute path
    // (directory + separator + stem + extension). 240 rather than Windows'
    // own 260 MAX_PATH: the created asset's siblings -- an Intermediate/
    // Artifacts/ path keyed off it, a ".meta" sidecar -- are longer than the
    // asset itself, so the asset needs headroom under the real ceiling, not
    // to sit exactly on it.
    inline constexpr std::size_t kCreateNameMaxPathChars = 240;

    [[nodiscard]] inline CreateNameCheck ValidateCreateName(std::string_view name,
                                                            const std::filesystem::path& targetDir,
                                                            std::string_view extension)
    {
        // ---- Rule 0/1: syntax. A pure string scan, first because it is the
        // cheapest and because a name that cannot be a file name at all makes
        // every later question meaningless.
        if (name.empty())
            return { false, "enter a name" };

        // The deny-set, verbatim: \ / : * ? " < > | -- the characters Windows
        // refuses in a file name, which is also the set POSIX's `/` is a
        // subset of, so one list serves both. Path separators are IN the set
        // on purpose: a name is a LEAF, never a sub-path (the Location combo
        // is how a different directory is chosen).
        constexpr std::string_view kDenied = "\\/:*?\"<>|";
        if (name.find_first_of(kDenied) != std::string_view::npos)
            return { false, "a name cannot contain any of  \\ / : * ? \" < > |" };

        // Leading/trailing dots and spaces: Windows SILENTLY STRIPS both when
        // it creates the file, so accepting them would mint an asset under a
        // name the user did not type -- and the uniqueness check below would
        // have asked about the un-stripped spelling, so it could not even
        // catch the resulting collision.
        const auto isDotOrSpace = [](char c) { return c == '.' || c == ' '; };
        if (isDotOrSpace(name.front()) || isDotOrSpace(name.back()))
            return { false, "a name cannot start or end with a dot or a space" };

        // ---- Rule 2: length. Arithmetic only -- no filesystem call yet.
        // Measured in NATIVE path characters (wchar_t on Windows) rather than
        // through path::string(), which converts through the active codepage
        // and can THROW on a name this function is specifically supposed to
        // return a verdict about.
        const std::size_t total = targetDir.native().size() + 1u   // + separator
                                + name.size() + extension.size();
        if (total >= kCreateNameMaxPathChars)
            return { false, "that name makes the full path too long ("
                            + std::to_string(total) + " of "
                            + std::to_string(kCreateNameMaxPathChars) + " characters)" };

        // ---- Rule 3: uniqueness. LAST -- the only rule that touches the
        // filesystem. Its message is deliberately UNLIKE the two above: it
        // names the colliding asset and what kind it is, so the fix ("pick
        // another name" / "open the one that is already there") reads
        // straight off the text.
        std::error_code ec;
        const std::filesystem::path target =
            targetDir / (std::string(name) + std::string(extension));
        if (std::filesystem::exists(target, ec))
            return { false, std::string("a ") + CreateNounForExtension(extension)
                            + " named " + std::string(name) + " already exists here" };

        return { true, {} };
    }

    // ---- the modal --------------------------------------------------------

    // Modal state, owned by EditorApp between frames (a modal spans frames;
    // its in-progress fields cannot live on the draw's stack). `open` is what
    // EditorApp::BeginCreateAsset sets and the dialog clears.
    struct CreateDialogState
    {
        bool open = false;
        CreateAssetRequest request{};
        char name[128] = {};
        int  folderIndex = 0;       // into the folder combo
        int  surface = 0;           // Material kind combo
        Arcane::Guid parent, texture;
        bool setAsBoot = false;
        bool pickerOpen = false;
    };

    // What a completed dialog hands back. `folder` is relative to Content/
    // ("materials", or "" for Content/ itself) -- the dispatcher joins it onto
    // the project's own content root, which is the only place a created asset
    // can register and resolve by GUID (Project.cpp:229: "game" -> root /
    // "Content").
    struct CreateAssetResult
    {
        CreateAssetKind kind = CreateAssetKind::Material;
        std::string name; std::string folder;    // relative to Content/
        int surface = 0;                          // Material: MaterialSurface value
        Arcane::Guid parent, texture; bool setAsBoot = false;
    };

    // Draw the modal for `st.request.kind`. Returns a completed result the
    // frame Create was clicked, nullopt otherwise (including every frame the
    // dialog is merely up). Cancel / Escape / the title bar's x close it and
    // return nullopt.
    //
    // TASK 12 SCOPE: Material and MaterialInstance carry their full field set.
    // Mesh/Sprite/Scene requests open the SHARED Name + Location anatomy with
    // Create disabled and one dim line saying the rest arrives next -- chosen
    // over refusing to open at all so the request plumbing every producer just
    // gained is VISIBLY end-to-end rather than a silent no-op (spec s13's own
    // rule against silent failure), at the cost of two lines this task's
    // successor deletes.
    std::optional<CreateAssetResult> DrawCreateAssetDialog(CreateDialogState& st,
                                                           const AssetPanelModel& model,
                                                           const Arcane::Project& project);
}
