#pragma once

// Shared contracts and chrome for the three asset panels (panel-split spec
// s5). One actions type, one services type, one create menu -- every panel
// returns/consumes the same shapes so the host consumes them identically.

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetActivityLog;
    class AssetPanelModel;
    // AssetsPanel.hpp -- forward-declared rather than included: that header
    // already includes THIS one (AssetPanelActions/Services + the create
    // menu), so pulling it in here would be circular. RevealAssetInBrowser
    // below only needs a reference to the type; AssetPanelCommon.cpp, which
    // has the real definition to call into, includes AssetsPanel.hpp itself.
    struct AssetsPanelState;
    // AssetPanelModel.hpp -- forward-declared for the same reason: only a
    // pointer type is needed below (ScenesByName's return), and pulling the
    // full header in here is not required for that.
    struct AssetPanelEntry;
    // AssetPanelModel.hpp's CookState enum (fixed std::uint8_t underlying
    // type, forward-declarable the same way a scoped enum with an explicit
    // base always is) -- CookStateLabel below only needs the TYPE for its
    // parameter, not the enumerators, so pulling in the model header for it
    // is not required.
    enum class CookState : std::uint8_t;
    // Documents/DocumentHost.hpp -- forward-declared for OpenAssetRow below,
    // which only needs a reference to the type; the definition it calls into
    // (AssetsPanel.cpp) already includes the real header.
    class DocumentHost;

    // Row/menu actions the APP resolves after the draw -- same "panel
    // reports, app performs" split the old (retired) AssetBrowserActions used
    // (dialogs and file IO never happen inside the panel draw). A superset of
    // that retired struct: adds `copyGuid` (spec s6's new context-menu entry)
    // and the unified-create pair (`requestCreateKind`/`createPrefillParent`,
    // Task 12). Task 9's placeholder body never raised the row-action fields
    // (no rows existed yet -- Task 10 draws them), but the struct carried the
    // full shape from the start so later tasks could extend this exact
    // contract rather than a new one.
    struct AssetPanelActions   // superset of the old (retired) AssetBrowserActions
    {
        Arcane::Guid createInstanceOf, createSpriteFrom, setBootScene,
                     showInExplorer, copyPath, copyGuid;
        std::filesystem::path openScene;
        // Unified create (Task 12): request the create dialog for a kind.
        // -1 = none. Values are **CreateAssetKind** (Panels/CreateAssetDialog.hpp)
        // -- NOT AssetKind, which numbers differently. A producer starting
        // from an AssetKind (the rail's per-kind "+") converts through
        // CreateKindForAssetKind before writing here; this field never
        // carries a raw AssetKind.
        int  requestCreateKind = -1;
        Arcane::Guid createPrefillParent;   // instance parent / sprite texture prefill

        // Plan 2 Task 7 (Status lens): the two needs-attention card buttons,
        // reported under exactly the same split as every field above -- the
        // panel never invalidates an artifact and never touches panel
        // visibility itself.
        //
        // `recook` names the refused asset the user asked to re-cook. The
        // host's consumer is pinned by the plan's Ruling 8:
        // InvalidateArtifact + ERASE that guid's cook-diagnostic row +
        // PublishCookDiagnostics + CookQueue::NoteChanged + MarkDirty --
        // erasing the row is what flips the card Refused -> Queued honestly
        // (with no row, IsCookPending re-derives the state from the artifact
        // store on the source's current cook key), and a source that still
        // cannot cook re-fails and puts the row back.
        //
        // `showProblems` asks the host to surface the Problems pane and
        // NOTHING more (Ruling 9 / spec s9.2 verbatim: "jumps to the pane").
        // Deliberately not a guid: no pre-filtering is specified, so none is
        // invented.
        Arcane::Guid recook;
        bool         showProblems = false;

        // Panel-split spec s7.1 (Task 3): three former DIRECT `state.lens`
        // writers -- the digest chip, the Unreferenced card's Reveal
        // button, and the Scenes card's "Focus in Graph" -- promoted to
        // actions for the same reason `showProblems` already is one: once
        // the split lands (Task 7) these cross INTO another panel's window,
        // and "the panel mutates a sibling panel's state directly" is
        // exactly the layering wart `showProblems`'s own header comment
        // never allowed for the Problems pane. Each raise site gates itself
        // on the matching AssetPanelServices bool below BEFORE writing here
        // (R1/s7.3: focus if open, else disabled) -- a consumer does not
        // need to re-derive that gate, only perform the effect.
        bool         showStatus = false;   // digest chip (Browse/Graph -> Status)
        Arcane::Guid revealInBrowse;       // Unreferenced card -> Browse
        Arcane::Guid focusInGraph;         // Scenes card -> Graph
    };

    // The Assets panel's read-only host seams. Originally just the
    // thumbnail resolver (Plan 1 Task 7's AssetServices,
    // consumed here per its own header comment: "Task 9's AssetPanelServices
    // consumes this exact callable"). Guid -> an ImGui texture id via the
    // chrome context's texture cache; 0 = unavailable, caller falls back to
    // the kind icon. Task 9's placeholder body never calls this; Task 10's
    // rows are the first consumer. Plan 2 Task 7 added the two Status-lens
    // seams beside it (see each field); every one of them is a READ the host
    // answers -- effects still travel the other way, through
    // AssetPanelActions.
    struct AssetPanelServices
    {
        std::function<std::uint64_t(const Arcane::Guid&)> resolveAssetThumb;

        // Plan 2 Task 7 (Status lens): the refusal DETAIL line for one guid,
        // resolved by the HOST out of its own cook-diagnostic accumulator
        // (EditorApp::m_cookDiagnostics -- detail, or message when the
        // diagnostic carries no detail). `nullopt` when there is no PERMANENT
        // row for the guid, in which case the card falls back to the bare
        // "cook refused" line. A seam rather than a direct read for the same
        // reason resolveAssetThumb is one: this panel compiles with zero
        // knowledge of Arcane::Diagnostic or the host's bookkeeping.
        std::function<std::optional<std::string>(const Arcane::Guid&)> cookDetailFor;

        // Plan 2 Task 5's session activity ring, borrowed non-owning (the
        // host owns it for the whole session; it is Clear()ed, never
        // destroyed, on a project switch). Wired here in Task 7 so the
        // services contract lands in one edit; Task 8's activity feed is its
        // first reader. May be null -- a caller must guard.
        const AssetActivityLog* activity = nullptr;

        // Panel-split spec s7.3 (Task 3): host-filled every frame from
        // m_panelVis (the same site resolveAssetThumb/cookDetailFor/
        // activity above are already filled at), so a raise site can grey
        // its own control + explain when its target is closed rather than
        // opening it (R1: nothing opens a panel except the Window menu).
        // The first three alias ONE panel (`PanelId::Assets`) until Task 7
        // gives Browse/Graph/Status separate ids -- today they can only
        // ever read identically, since there is exactly one "Assets"
        // window housing all three lenses. `problemsOpen` is not an alias:
        // Problems is already its own panel, and gates the Attention
        // card's "Problems" button under the same rule.
        bool browserOpen = false, graphOpen = false, statusOpen = false, problemsOpen = false;
    };

    // The unified Create menu's entries (spec s7), spelled ONCE and shared
    // by the toolbar's `+ Create` popup and every row's context-menu
    // "Create" submenu -- the invariant ("no creation path may bypass
    // CreateAssetRequest") is only cheap to hold if there is one list.
    //
    // `enabled` was the only difference between the two call sites while
    // Mesh/Sprite/Scene had no dialog fields to land on (Task 12): the
    // toolbar's entries went live then, the row context menu's stayed
    // disabled. Both are live as of Task 13 -- kept as a parameter rather
    // than collapsed to a bare call so a future producer (Plan 3's graph
    // pin-drag) can still gate itself the same way without a third copy
    // of this list. No per-row prefill flows through here: a row's own
    // "Create -> Sprite..." does not pre-pick THIS row's texture (the
    // dedicated "Create Sprite" quick action above it already covers
    // that exact case, mint-or-reuse and open included) -- the generic
    // submenu opens the SAME dialog the toolbar's `+ Create` does, empty
    // texture field and all.
    void DrawCreateMenuEntries(AssetPanelActions& actions, bool enabled);
    void DrawCreateMenu(AssetPanelActions& actions);

    // Panel-split spec s7.2 (Task 3): today's Reveal sequence (the
    // Unreferenced card's own click handler, pre-split), extracted to a
    // free function so the host's `revealInBrowse` consumer can run it
    // without one panel reaching into a sibling's state -- clears search
    // and the kind filter in BOTH places (state mirror + model), walks
    // `guid`'s folder ancestry forcing every group open in both places,
    // forces the derived fold open when `foldedUnder` is valid, then
    // selects. A no-op when `guid` no longer resolves to an entry (the
    // action may be consumed a frame after it was raised, and the model
    // can have moved on in between). `state` is Browse's own state mirror
    // -- Task 7 retargets this parameter to `AssetBrowserPanelState&` once
    // Browse is its own panel; nothing else about the contract changes.
    void RevealAssetInBrowser(AssetsPanelState& state, AssetPanelModel& model,
                              const Arcane::Guid& guid);

    // Panel-split Task 4: four more lens-shared helpers, promoted here for
    // exactly the reason RevealAssetInBrowser above already was -- Browse
    // and/or Graph call each of these from AssetsPanel.cpp, and the Status
    // lens's body (AssetStatusPanel.cpp, its own TU as of Task 4) needs to
    // reach the SAME four, not a second copy that could drift. Every one of
    // the four keeps its body exactly where it was (AssetsPanel.cpp, still
    // the one place any `ed::`/anonymous-namespace-sibling call it makes
    // can resolve) -- only the enclosing namespace brace moved, from
    // AssetsPanel.cpp's anonymous namespace (internal linkage, one TU only)
    // out to here (external linkage, every TU that includes this header).

    // The project's recorded boot scene as a guid -- see the definition's
    // own comment (AssetsPanel.cpp) for the full rationale. Nil for a null
    // project or an empty/unparseable bootScene.
    Arcane::Guid BootSceneGuid(const Arcane::Project* project);

    // Every Scene entry, name-sorted (ties broken on mount path) -- see the
    // definition's own comment (AssetsPanel.cpp): the Status lens's Scenes
    // rollup and the Graph lens's focus combo share this ONE list, in this
    // ONE order.
    std::vector<const AssetPanelEntry*> ScenesByName(const AssetPanelModel& model);

    // The unified asset peek tooltip (spec s8) -- thumb, name, kind/subkind/
    // instance pills, mount path, cook state, guid, and (Graph lens only)
    // an edge-summary line. See the definition's own comment
    // (AssetsPanel.cpp) for `forceShow`/`withEdgeSummary`.
    void DrawAssetPeekTooltip(const AssetPanelModel& model, const AssetPanelServices& services,
                              const Arcane::Guid& guid, bool forceShow = false,
                              bool withEdgeSummary = false);

    // Panel-split Task 5: three MORE lens-shared helpers, found when the
    // Graph lens's body moved out to its own TU (AssetGraphPanel.cpp) --
    // Task 4's four above were the ones the Status split already needed;
    // these three are calls the Graph body makes that Browse's code in
    // AssetsPanel.cpp still needs too. Same promotion, same reason: each
    // keeps its body exactly where it was (AssetsPanel.cpp) and only the
    // enclosing namespace brace moved, from an anonymous namespace out to
    // here.

    // Resolve + route a double-click / Enter-open -- see the definition's
    // own comment (AssetsPanel.cpp) for the exact routing (a scene goes
    // through `actions.openScene`, everything else through `docs`). A
    // Browse row's double-click and the Graph lens's node double-click both
    // call this, one copy.
    void OpenAssetRow(const AssetPanelEntry& e, const Arcane::Project* project,
                      DocumentHost& docs, AssetPanelActions& actions);

    // The unified asset context menu's ITEMS (spec s6) -- see the
    // definition's own comment (AssetsPanel.cpp) for why this carries no
    // popup bracket of its own. A Browse row's context menu and the Graph
    // lens's node context menu both call this, one copy.
    void DrawAssetMenuItems(AssetPanelActions& actions, const AssetPanelEntry& e,
                            bool kindSpecific);

    // Materials-only subkind pill text (spec s3.1/s6) -- see the
    // definition's own comment (AssetsPanel.cpp). A Browse row's pill, the
    // preview pane's pill and the Graph lens's node body pill all read this
    // same text.
    const char* SubkindPillText(const AssetPanelEntry& e);

    // Panel-split Task 6: a SIXTH lens-shared helper, found when the Browse
    // lens's body moved out to its own TU (AssetBrowserPanel.cpp) -- the
    // preview pane's cook row and DrawAssetPeekTooltip's own cook line (both
    // still AssetsPanel.cpp's, the tooltip's body unmoved) format the same
    // CookState the same way. Same promotion as the five above: the body
    // keeps living exactly where it was (AssetsPanel.cpp), only the
    // enclosing namespace brace moved.
    //
    // CookState-to-display-string (spec s6/s8: "Cooked"/"Queued"/"Refused"/
    // "Unknown") -- see the definition's own comment (AssetsPanel.cpp).
    const char* CookStateLabel(CookState cook);

    // AssetPill's own width, WITHOUT drawing it (EditorWidgets.cpp's
    // AssetPill, 12px text plus its two FramePadding.x cheeks) -- a caller
    // that positions a pill by hand needs the width one item early to
    // budget an ellipsis against it. Cross-lens for the same reason as the
    // three above: the Graph lens's node chrome and (before Task 4) the
    // Status lens's attention card both call it.
    float PillWidth(const char* text);

    // Toolbar / bottom bar band heights (spec s11.2's values table:
    // "toolbar wells / bottom bar | 24px / 24px"). The default ImGui
    // frame (Inter 16px body over the theme's untouched FramePadding.y=3,
    // EditorTheme.hpp's own comment) stands 22px tall; bumping
    // FramePadding.y to 4 for just the toolbar row (pushed/popped around
    // its controls) is what closes the last 2px to the pinned 24.
    inline constexpr float kAssetPanelToolbarFramePadY = 4.0f;
    inline constexpr float kAssetPanelBottomBarHeight  = 24.0f;
    // 2026-09-07 fix (mock parity, automation-measured): the vertical
    // gap between the toolbar row's bottom edge and the Browse body's
    // top edge, pixel-scanned off `OptionBC-Browse-FINAL.png` (7px of
    // pure background between the toolbar's own bottom border and the
    // body's own top border -- 61->69 border-to-border at the mock's
    // native resolution). No §5/§11.2 value was previously pinned for
    // this seam -- see DrawAssetsPanel's own comment for why it had
    // silently collapsed to 0px live.
    inline constexpr float kAssetPanelToolbarBodyGapPx = 7.0f;

    // Task 10 (spec s6/s11.2) row pitch, promoted here in Task 4 alongside
    // BootSceneGuid/ScenesByName/DrawAssetPeekTooltip above: the Status
    // lens's Unreferenced card (Plan 2 Task 8) draws its rows at this exact
    // pitch, matching every Browse table row (AssetsPanel.cpp's own Task 10
    // fixed-geometry block, unchanged) -- an `inline constexpr` rather than
    // a second copy of the literal, the same avoid-drift reasoning every
    // other constant on this header already follows.
    inline constexpr float kTableRowHeight = 24.0f;
}
