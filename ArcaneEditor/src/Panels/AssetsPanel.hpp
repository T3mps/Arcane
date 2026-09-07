#pragma once

// AssetsPanel (asset-manager redesign, Plan 1 Task 9): the panel that
// REPLACES AssetBrowser.hpp's DrawAssetBrowserPanel as what the "Assets" tab
// draws. This task ships the SHELL ONLY -- the three fixed bands spec S5
// pins (toolbar / body / bottom bar) and the verbatim state/actions/services
// contracts Tasks 10-13 extend IN PLACE. The Browse lens's real body (rail +
// grouped table + preview pane) lands in Task 10; until then the body is a
// placeholder child region.
//
// Panel identity is UNCHANGED from the old panel: same "Assets" ImGui::Begin
// title (PanelRegistry.hpp:37, imgui.ini keys untouched), same PanelId::Assets
// visibility flags -- no dock churn.
//
// AssetKind/AssetEntry/MatchesFilter etc. (originally AssetBrowser.hpp's
// classification vocabulary; migrated into AssetPanelModel.hpp in Task 15,
// see that header's own comment) stay the classification vocabulary
// underneath AssetPanelModel -- this file adds no new classification, only
// the panel shell.

#include "Panels/AssetPanelModel.hpp"   // AssetPanelModel (current before every panel draw)

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class DocumentHost;
    // Plan 2 Task 5's session activity ring. Forward-declared (same shape as
    // DocumentHost above) rather than included: this header only needs to name
    // a POINTER to one -- Task 8's feed, the first reader, includes the real
    // header in the .cpp.
    class AssetActivityLog;

    // Which lens the panel shows. Plan 1 ships Browse only -- Graph (Plan 3)
    // and Status (Plan 2) exist in the enum and in the toolbar's lens strip
    // from day one (layout pinned per spec s5: "later plans enable, nothing
    // shifts"), but both stay disabled until their own plan lands.
    enum class AssetLens : std::uint8_t { Browse, Graph, Status };

    // The preview pane's default width (2026-09-07 follow-up). Lives here,
    // not as a second literal duplicated in AssetsPanel.cpp, so
    // AssetsPanelState's own field default below and the splitter's
    // double-click-reset target (AssetsPanel.cpp) can never drift apart --
    // a review minor on the first cut of this feature, where both spellings
    // independently hardcoded 165.0f.
    inline constexpr float kAssetsPreviewPaneDefaultWidth = 165.0f;

    // Session-only UI state (spec s5: panel state is session-only in v1).
    // `search` feeds AssetPanelModel::SetSearch every frame. `railKind` is
    // unused until Task 10 wires the rail -- left at -1 (All) so feeding it
    // to SetKindFilter today is a no-op. `seenSelectionStamp` lets a later
    // task scroll-to-selection exactly once by comparing against
    // model.selectionStamp.
    struct AssetsPanelState
    {
        AssetLens lens = AssetLens::Browse;
        char search[128] = {};
        int  railKind = -1;                 // -1 = All
        std::uint32_t seenSelectionStamp = 0; // scroll-to-selection once

        // 2026-09-07 follow-up (spec s5/s11.2 addendum, post-Task-11): the
        // preview pane's DESIRED width, user-resizable via a drag splitter
        // between the table and the pane. Session-only, same convention as
        // every other field here -- NOT persisted to imgui.ini (contrast the
        // Material panel's ShaderEditorDocument PaneSplitter ratio, which IS
        // persisted; this one deliberately is not).
        //
        // "Desired", precisely: this field is written ONLY by the splitter's
        // drag and its double-click reset (both in AssetsPanel.cpp) -- never
        // by the per-frame layout clamp, which computes a separate, purely
        // local DRAWN width instead (ClampPreviewForLayout). A review fix
        // (2026-09-07): the first cut clamped this field itself every frame,
        // which meant a transient panel-narrowing (a window resize, nothing
        // the user asked of the pane) silently and PERMANENTLY reduced
        // whatever the user had actually dragged to, with no way back once
        // the panel widened again. Splitting "what the user wants" from
        // "what fits on screen this frame" is what fixes that: the wide
        // value survives the narrow interval untouched and reasserts itself
        // the moment there is room again.
        float previewPaneWidth = kAssetsPreviewPaneDefaultWidth;

        // Task 10: session-only fold/group open state, MIRRORING
        // AssetPanelModel's own private m_groupOpen/m_childrenOpen (same
        // defaults: a folder absent from `groupOpen` is OPEN, a texture
        // guid absent from `childrenOpen` is COLLAPSED). The model exposes
        // no getter for either -- Rows() already bakes the effective result
        // into which rows exist -- but the panel still needs to know which
        // glyph to draw (chevron open/closed) and what to flip, so it keeps
        // its own copy and pushes every toggle through
        // AssetPanelModel::SetGroupOpen/SetChildrenOpen (the only two
        // writers of the model's maps), which keeps the two in lockstep by
        // construction rather than by convention.
        std::unordered_map<std::string, bool>  groupOpen;
        std::unordered_map<Arcane::Guid, bool> childrenOpen;
    };

    // Row/menu actions the APP resolves after the draw -- same "panel
    // reports, app performs" split the old (retired) AssetBrowserActions used
    // (dialogs and file IO never happen inside the panel draw). A superset of
    // that retired struct: adds `copyGuid` (spec s6's new context-menu entry)
    // and the unified-create pair (`requestCreateKind`/`createPrefillParent`,
    // Task 12). Task 9's placeholder body never raised the row-action fields
    // (no rows existed yet -- Task 10 draws them), but the struct carried the
    // full shape from the start so later tasks could extend this exact
    // contract rather than a new one.
    struct AssetsPanelActions   // superset of the old (retired) AssetBrowserActions
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
        // (IsCookPending presumes pending on an absent row), and a source
        // that still cannot cook re-fails and puts the row back.
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
    // consumed here per its own header comment: "Task 9's AssetsPanelServices
    // consumes this exact callable"). Guid -> an ImGui texture id via the
    // chrome context's texture cache; 0 = unavailable, caller falls back to
    // the kind icon. Task 9's placeholder body never calls this; Task 10's
    // rows are the first consumer. Plan 2 Task 7 added the two Status-lens
    // seams beside it (see each field); every one of them is a READ the host
    // answers -- effects still travel the other way, through
    // AssetsPanelActions.
    struct AssetsPanelServices
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

    // Draw the "Assets" panel: toolbar (+ Create / search / lens strip) ·
    // body (the active lens; Browse is a placeholder child until Task 10) ·
    // bottom bar (context + digest, spec s5). `model` is rebuilt by the
    // caller (RebuildIfDirty) BEFORE this runs every frame -- this panel only
    // reads it, plus feeds this frame's toolbar edits back in
    // (SetSearch/SetKindFilter). `open` is forwarded to ImGui::Begin (the
    // tab's X button; null = no X).
    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& docs,
                                       const AssetsPanelServices& services,
                                       bool* open = nullptr);
}
