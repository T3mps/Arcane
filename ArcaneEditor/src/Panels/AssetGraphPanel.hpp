#pragma once

// AssetGraphPanel (panel-split arc): the "Asset Graph" window -- the focus-
// combo toolbar, the ax::NodeEditor canvas body, its lifecycle, and the
// bottom bar. Task 5 moved the body here as pure motion out of
// AssetsPanel.cpp's DrawGraphLens; Task 7 wrapped it in its own panel shell,
// brought the focus combo across from the retired shared toolbar, and gave it
// the half of AssetsPanelState it actually reads. See AssetGraphPanel.cpp's
// own header comment for the full accounting.
//
// Exports: DrawAssetGraphPanel (the window), DrawAssetGraphBody (the canvas
// body it wraps), DestroyAssetGraphPanelCanvas (the host's project-switch +
// shutdown seam) and SeedAssetGraphFocus (the boot-scene seed the wrapper
// runs before its toolbar -- see its own comment).
//
// AssetsGraphProjectionIsCurrent is GONE as of Task 7 (spec s7.4): it existed
// only because a same-frame LENS FLIP could put the Graph bottom bar on
// screen in a frame whose body was another lens's. With one window per view
// the bar draws inside this panel's own Begin/End, after this panel's own
// body -- the stale-projection frame is unrepresentable, so the predicate is
// deleted rather than moved.

#include "Panels/AssetGraphViewModel.hpp"   // AssetGraphViewModel -- the built projection state caches
#include "Panels/AssetPanelCommon.hpp"      // AssetPanelActions/AssetPanelServices, BootSceneGuid
#include "Widgets/GraphGridPhase.hpp"       // GraphGridPhase -- ImGui-only, no node-editor coupling

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <optional>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;

    // The Asset Graph window's session-only UI state (spec s6). Panel-split
    // Task 7: the other of the two structs AssetsPanelState dissolved into,
    // carrying exactly the fields the Graph body, its focus combo and its
    // bottom bar read -- the partition the original struct's own "Plan 3
    // (Graph lens) session state" comment already drew. Every field comment
    // below is carried over verbatim from AssetsPanelState.
    struct AssetGraphPanelState
    {
        // The ax::NodeEditor canvas context, held as an OPAQUE pointer ON
        // PURPOSE: plan ruling 1 keeps every `ed::` call panel-local to
        // AssetGraphPanel.cpp, so this header must never include
        // imgui_node_editor.h (the same refusal CanvasPopupScope.hpp:16-19
        // already makes for the shared widget layer). Created lazily by the
        // first Graph draw; destroyed through DestroyAssetGraphPanelCanvas
        // (below) -- NEVER by a caller that reaches in and casts, which would
        // need the header this field exists to avoid.
        void* graphCanvas = nullptr;
        // Per-canvas grid phase (one instance per canvas, exactly as the
        // shader editor keeps one per document canvas).
        GraphGridPhase graphGrid;
        // The graph's scope root. NIL = "everything" (ruling 6: there is no
        // root to measure from). Seeded from the boot scene by
        // SeedAssetGraphFocus; edited by the toolbar combo.
        Arcane::Guid graphFocus;
        // Nil-focus kind gate (`@source` etc.). Ignored when graphFocus is a
        // real guid. Cleared by picking "everything" or a named asset.
        std::optional<AssetKind> graphKindFilter;
        // Filter buffer for the typeable focus combo. Lives only while the
        // popup is open; zeroed when it closes.
        char graphFocusFilter[128] = {};
        int  graphFocusNav = 0;        // highlighted row in the open combo
        int  graphFocusFilterLen = 0;  // BufTextLen last callback; shrinking skips autofill
        // Task 5: has `graphFocus` been seeded from THIS project's boot scene
        // yet? A separate flag rather than "is graphFocus nil": nil is a
        // LEGITIMATE user choice (the combo's own "everything" entry), and
        // re-seeding the boot scene over it on the next frame would make that
        // entry unpickable. Cleared by DestroyAssetGraphPanelCanvas -- the
        // panel's project-switch seam -- so the next project seeds its OWN boot scene.
        // Only ever set with a project in hand, so a project-less boot does not
        // burn the seed on a nil manifest.
        bool graphFocusSeeded = false;
        // Ruling 4: the Graph view gets its OWN selection stamp -- sharing
        // the Browser's `seenSelectionStamp` would let one consumer swallow
        // the other's pending scroll/center. Now that the two live in
        // separate structs the separation is structural as well as nominal,
        // and the spelling is kept so the moved body reads unchanged.
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
        // delay that the Graph canvas cannot use (see DrawAssetGraphBody's
        // tooltip block: ImGui's "last item" out there is the canvas, never
        // the node, so the hover authority is the node editor's own hit test
        // and the helper is called with `forceShow`). `graphHoverGuid` is the
        // ASSET the timer is running for -- a change resets the clock,
        // exactly as ImGui resets its own delay when the hovered item changes.
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
        std::optional<AssetKind> graphBuiltKindFilter;
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

    // Seed `graphFocus` to the project's boot scene, once per project,
    // BEFORE anything reads it this frame -- Task 5 round 1 fix. The seed
    // used to run inside DrawAssetGraphBody's own preamble, which executes
    // AFTER the toolbar: the focus combo would read the STILL-unseeded
    // `graphFocus` on the one frame a project opens, while the bottom bar --
    // which runs after the body -- read the freshly-seeded value the same
    // frame, so the toolbar said "focus: everything" and the bottom bar said
    // "focus: <boot scene>" in the same frame. Exported as a small inline
    // helper so DrawAssetGraphPanel can call it at the seam that restores the
    // pre-split execution order in effect, not just the code's physical
    // location. Task 7 moved that seam one line EARLIER still -- above the
    // window's own collapse early-out, since a buried tab must not leave the
    // latch unspent for a later deep link to be overwritten by; see the call
    // site's own comment for the full account.
    //
    // Idempotent per project: `graphFocusSeeded` (cleared by
    // DestroyAssetGraphPanelCanvas's project-switch reset) guards the write,
    // so a user's own "everything" pick on the toolbar combo is never
    // silently re-seeded back to the boot scene on a later frame.
    inline void SeedAssetGraphFocus(AssetGraphPanelState& state, const Arcane::Project* project)
    {
        if (project && !state.graphFocusSeeded)
        {
            state.graphFocus       = BootSceneGuid(project);
            state.graphFocusSeeded = true;
        }
    }

    // Draw the Graph canvas body: the ax::NodeEditor canvas, its lazily-created
    // context, the built AssetGraphViewModel projection (rebuilt only when
    // the focus or the model's entries moved), layered nodes, two-layer
    // kind-coloured edges, the selection bridge, the peek tooltip with its
    // edge summary, double-click open, the unified context menu, and the
    // pin-drag "Derive Instance..." gesture with its dashed in-flight wire
    // and the canvas legend. `model`/`project` are read; every effect
    // travels through `actions`, gated by `services`; `docs` routes a
    // non-scene open the same way a Browser row's does. The boot-scene focus
    // seed does NOT happen in here -- see SeedAssetGraphFocus above, and its
    // caller in DrawAssetGraphPanel, for why it has to run before this body
    // does. See the definition's own comment (AssetGraphPanel.cpp) for the
    // full section-by-section accounting.
    void DrawAssetGraphBody(AssetGraphPanelState& state, AssetPanelModel& model,
                            const Arcane::Project* project, DocumentHost& docs,
                            const AssetPanelServices& services,
                            AssetPanelActions& actions);

    // Draw the "Asset Graph" window (panel-split spec s5/s9): toolbar (the
    // focus combo ONLY -- no Create, no search, spec s9.1's R2 minimum) ·
    // body · bottom bar ("X of N assets - focus: <scene>" / the health digest
    // chip, spec s9.2). `model` is rebuilt by the caller before this runs
    // every frame; this panel only reads it. `open` is forwarded to
    // ImGui::Begin (the tab's X button; null = no X).
    AssetPanelActions DrawAssetGraphPanel(AssetGraphPanelState& state, AssetPanelModel& model,
                                          const Arcane::Project* project, DocumentHost& docs,
                                          const AssetPanelServices& services,
                                          bool* open = nullptr);

    // Tear the Graph canvas context down and drop the built projection with
    // it (Plan 3 Task 3; renamed from DestroyAssetsPanelCanvas in Task 5,
    // panel-split). The HOST calls this at exactly two seams -- a project
    // switch and shutdown -- see the definition's own comment
    // (AssetGraphPanel.cpp) for why a function here rather than an
    // `ed::DestroyEditor` at those call sites, and for the MUST-run-while-an-
    // ImGui-context-is-current rule. Idempotent, and safe when the panel was
    // never opened.
    void DestroyAssetGraphPanelCanvas(AssetGraphPanelState& state);
}
