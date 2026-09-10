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

namespace Arcane::Editor
{
    class AssetActivityLog;

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
}
