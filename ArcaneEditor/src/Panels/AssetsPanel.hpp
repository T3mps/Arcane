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

#include "Panels/AssetGraphViewModel.hpp"   // the Graph lens's built projection (state caches one)
#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/Services + create menu + shared chrome constants
#include "Panels/AssetPanelModel.hpp"   // AssetPanelModel (current before every panel draw)
#include "Widgets/GraphGridPhase.hpp"   // GraphGridPhase -- ImGui-only, no node-editor coupling

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class DocumentHost;

    // Which lens the panel shows. Plan 1 shipped Browse only -- Graph (Plan 3)
    // and Status (Plan 2) existed in the enum and in the toolbar's lens strip
    // from day one (layout pinned per spec s5: "later plans enable, nothing
    // shifts"), each disabled until its own plan landed. All three are live
    // as of Plan 3 Task 5, and the strip's layout never moved for any of them.
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

        // ---- Plan 3 (Graph lens) session state -------------------------
        // The ax::NodeEditor canvas context, held as an OPAQUE pointer ON
        // PURPOSE: plan ruling 1 keeps every `ed::` call lens-local to
        // AssetsPanel.cpp, so this header must never include
        // imgui_node_editor.h (the same refusal CanvasPopupScope.hpp:16-19
        // already makes for the shared widget layer). Created lazily by the
        // first Graph-lens draw; destroyed through
        // DestroyAssetsPanelCanvas below -- NEVER by a caller that reaches
        // in and casts, which would need the header this field exists to
        // avoid.
        void* graphCanvas = nullptr;
        // Per-canvas grid phase (one instance per canvas, exactly as the
        // shader editor keeps one per document canvas).
        GraphGridPhase graphGrid;
        // The graph's scope root. NIL = "everything" (ruling 6: there is no
        // root to measure from). Task 5 seeds it from the boot scene and
        // adds the toolbar combo that edits it.
        Arcane::Guid graphFocus;
        // Task 5: has `graphFocus` been seeded from THIS project's boot scene
        // yet? A separate flag rather than "is graphFocus nil": nil is a
        // LEGITIMATE user choice (the combo's own "everything" entry), and
        // re-seeding the boot scene over it on the next frame would make that
        // entry unpickable. Cleared by DestroyAssetsPanelCanvas -- the panel's
        // project-switch seam -- so the next project seeds its OWN boot scene.
        // Only ever set with a project in hand, so a project-less boot does not
        // burn the seed on a nil manifest.
        bool graphFocusSeeded = false;
        // Ruling 4: the Graph lens gets its OWN selection stamp -- sharing
        // Browse's `seenSelectionStamp` would let one consumer swallow the
        // other's pending scroll/center. Task 4 is the consumer.
        std::uint32_t seenSelectionStampGraph = 0;

        // ---- Task 4 interaction state ----------------------------------
        // The asset a node context menu is OPEN about. A popup outlives the
        // one frame `ed::ShowNodeContextMenu` reports the gesture on
        // (imgui_node_editor.cpp: ContextMenuAction::Process clears the flag
        // every frame), so the guid has to survive between them -- the same
        // reason the shader editor's pass canvas keeps `m_passCtxNode`. The
        // guid rather than the node id on purpose: node ids are index+1 into
        // the CURRENT build and renumber on every rebuild, so an id stored
        // here would silently come to mean a different asset.
        Arcane::Guid graphMenuGuid;
        // Peek-tooltip dwell, standing in for the `ImGuiHoveredFlags_ForTooltip`
        // delay that the Graph lens cannot use (see DrawGraphLens's tooltip
        // block: ImGui's "last item" out there is the canvas, never the node,
        // so the hover authority is the node editor's own hit test and the
        // helper is called with `forceShow`). `graphHoverGuid` is the ASSET
        // the timer is running for -- a change resets the clock, exactly as
        // ImGui resets its own delay when the hovered item changes.
        //
        // The guid rather than the hovered node id, for exactly the reason
        // `graphMenuGuid` above is a guid: node ids renumber on every rebuild,
        // so an id key compares numerically EQUAL across a rebuild while the
        // asset behind it changes -- silently handing one asset's elapsed
        // dwell to another.
        Arcane::Guid  graphHoverGuid;
        float         graphHoverSeconds = 0.0f;

        // ---- Task 6: the pin-drag "Derive Instance..." gesture ----------
        // The asset a released pin-drag is ABOUT. Written on the ONE frame
        // ed::AcceptNewItem() reports the drop and read for as long as the
        // ghost menu stays open, so it has to outlive that frame -- and it is
        // a GUID for the same reason `graphMenuGuid` above is: node and pin
        // ids are derived from the node's INDEX in the current build and
        // renumber on every rebuild, so an id stashed across frames would
        // silently come to name a different asset (or none). A rebuild
        // landing mid-drag therefore cancels the gesture instead of
        // corrupting it -- the panel's id-resolution guard refuses the stale
        // pin id and the query is rejected.
        Arcane::Guid graphWireGuid;
        // ...and whether that drag can actually derive an instance: it
        // started from the DEPENDENTS (right) pin of a live MATERIAL. False
        // leaves the ghost menu's one entry DISABLED rather than hidden --
        // the gesture stays discoverable from any pin, it just cannot promise
        // something the source kind does not support.
        bool graphWireDerivable = false;
        // The IN-FLIGHT half: the asset a drag is currently leaving, and which
        // of its two pins it left by. Session state rather than a frame local
        // because the create query reports a dragged pin only on frames where
        // the pointer is over EMPTY canvas or over another pin -- crossing a
        // node BODY reports nothing at all, and a wire that blinks out every
        // time it passes behind a node is not a wire. Cleared on the first
        // frame ed::BeginCreate reports no live action, which is the frame
        // after the drag ends however it ended (released, cancelled, or
        // consumed by the accept).
        //
        // A guid again, re-resolved through the CURRENT build's guid->index map
        // every frame, so a rebuild landing mid-drag re-anchors the curve
        // instead of aiming it at whatever now occupies an old index.
        Arcane::Guid graphDragGuid;
        bool         graphDragRight = false;

        // The built projection plus the two inputs it was built from. The
        // dirty trigger is a stamp comparison, never a per-frame rebuild:
        // `graph` is re-Built only when AssetPanelModel::entriesStamp moved
        // (its entries/index changed) or the focus changed. See
        // AssetPanelModel::entriesStamp's own declaration for why that
        // counter -- and not RebuildIfDirty's return value -- is the honest
        // trigger.
        AssetGraphViewModel graph;
        std::uint32_t graphBuiltStamp = 0;
        Arcane::Guid  graphBuiltFocus;
        bool          graphBuilt = false;
        // Set whenever `graph` was rebuilt (or the canvas context was just
        // created) and consumed by the next canvas frame's
        // ed::SetNodePosition pass. Ruling 2: computed layout is written on
        // every REBUILD, not every frame -- which is what leaves the
        // library's own node dragging usable in between, with the explicit
        // contract that a reposition is TRANSIENT (the next rebuild snaps it
        // back). That is intended behavior, not a bug.
        bool          graphLayoutDirty = false;
    };

    // Draw the "Assets" panel: toolbar (+ Create / search / lens strip) ·
    // body (the active lens; Browse is a placeholder child until Task 10) ·
    // bottom bar (context + digest, spec s5). `model` is rebuilt by the
    // caller (RebuildIfDirty) BEFORE this runs every frame -- this panel only
    // reads it, plus feeds this frame's toolbar edits back in
    // (SetSearch/SetKindFilter). `open` is forwarded to ImGui::Begin (the
    // tab's X button; null = no X).
    AssetPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                      const Arcane::Project* project, DocumentHost& docs,
                                      const AssetPanelServices& services,
                                      bool* open = nullptr);

    // Is the Graph lens's cached projection CURRENT -- built for the focus
    // and the entries the panel would name this frame (Plan 3 Task 5, review
    // finding I1)?
    //
    // It is not always, and the gap is one frame wide. The Status lens's
    // "Focus in Graph" button flips `state.lens` to Graph from INSIDE the
    // already-dispatched Status body, so DrawGraphLens does not run that frame
    // at all -- while DrawBottomBar, which runs after the body, already reads
    // the NEW lens. Without this gate the bar would pair the PREVIOUS build's
    // node count (often 0: the lens may never have been opened) with the new
    // focus's name and print a confident lie that self-corrects one frame
    // later. Spec §13: never render an unknown as a zero -- unknown is an em
    // dash.
    //
    // Exported rather than left file-local to AssetsPanel.cpp for exactly one
    // reason: the panel's bottom bar and the device-less canvas test must ask
    // the SAME question. A test that restated the conjunction would keep
    // passing if the panel later dropped a conjunct -- precisely the
    // regression this predicate exists to prevent.
    [[nodiscard]] bool AssetsGraphProjectionIsCurrent(const AssetsPanelState& state,
                                                      const AssetPanelModel& model);

    // Tear the Graph lens's canvas context down and drop the built
    // projection with it (Plan 3 Task 3). The HOST calls this at exactly two
    // seams -- a project switch (beside AssetPanelModel::ResetForProjectSwitch:
    // a new project shares no reference topology, no node ids and no view with
    // the old one) and shutdown (before the ImGui context dies).
    //
    // Why a function here rather than an `ed::DestroyEditor` at those call
    // sites: plan ruling 1 pins every `ed::` call to AssetsPanel.cpp, and
    // `graphCanvas` is deliberately a `void*` for the same reason -- a caller
    // able to destroy it directly would need the node-editor header this
    // header exists to keep out. Idempotent, and safe when the lens was never
    // opened (the context is created lazily, so it is usually null).
    //
    // MUST run while an ImGui context is current: ~EditorContext touches only
    // ImGui/CPU state (the shader editor's own dtor comment,
    // ShaderEditorDocument.cpp), but it does touch it.
    void DestroyAssetsPanelCanvas(AssetsPanelState& state);
}
