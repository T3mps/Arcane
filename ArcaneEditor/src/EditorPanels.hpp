#pragma once

#include "EntityList.hpp"
#include "ViewportInput.hpp"
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <imgui.h>          // ImGuiID (InspectorState parks the gesture owner's item id)
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane { class RunLoop; class Runtime; class Project; struct PluginVTable; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class ConsoleBuffer;
    class PlaySession;
    struct SelectionContext;

    // Menu-bar requests the app resolves AFTER the frame's dockspace is drawn
    // (dialog launches happen at the call site, never inside the menu draw).
    struct MenuRequests
    {
        bool openProject = false;    // File -> Open Project...   (folder dialog)
        bool newMaterial = false;    // File -> New Material...   (save dialog; graph-owned)
        bool openMaterial = false;   // File -> Open Material...  (open-file dialog)
        bool newScene = false;       // File -> New Scene
        bool openScene = false;      // File -> Open Scene...     (open-file dialog)
        bool saveScene = false;      // File -> Save Scene        (Save As when never saved)
        bool saveSceneAs = false;    // File -> Save Scene As...  (save dialog)
    };

    // Open the full-viewport dockspace host window + the editor menu bar and LEAVE IT
    // OPEN (call once per frame right after ImGui BeginFrame). Draw the fixed toolbar
    // strip (DrawSimTimeToolbar) into it, then close it with EndDockSpace(); dockable
    // panels are drawn AFTER EndDockSpace. `undo` drives the Edit menu's Undo/Redo
    // (same CommandStack as the Ctrl+Z / Ctrl+Y shortcuts handled in the app input loop).
    // Menu clicks land in `requests`; the caller launches the dialogs.
    // `sceneDirty` puts the * on Save Scene; `playing` greys both Save items out.
    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing);

    // Emit the DockSpace() into the host window opened by BeginDockSpace and close it.
    // Everything drawn in between becomes a fixed (non-dockable, tab-less) strip above
    // the dockspace.
    void EndDockSpace();

    // Centered Play/Pause/Step transport, drawn as a FIXED STRIP into the current window
    // -- call between BeginDockSpace and EndDockSpace so it lands in the dockspace host.
    // Not its own window: no tab, cannot be docked or moved. Unity-style toggles (Play
    // tinted while playing, Pause tinted while paused + disabled outside Play, Step
    // disabled outside Play). Undo/Redo moved to the Edit menu; the gizmo tools moved to
    // the Viewport overlay. `plugin` is the hosted plugin's vtable (may be null):
    // Play/Stop route through its SaveState/LoadState so the plugin re-establishes its
    // native resources on restore. Does NOT take a RunLoop&: play.Stop() ->
    // Runtime::RestoreRegistry destroys and replaces the RunLoop, so the loop is fetched
    // fresh from `runtime` AFTER Play/Stop handling. `logoTex` is the Arcane logo as an
    // ImGui texture id (the raw nvrhi::ITexture* as a uint64_t, matching DrawViewportPanel's
    // convention); drawn as a small mark at the far LEFT of the strip (Unity-style). 0 skips it.
    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin, uint64_t logoTex = 0);

    // (The Assets panel is the REAL browser now -- AssetBrowser.hpp's
    // DrawAssetBrowserPanel; the placeholder stub retired in Slice 6.)


    // Scrolling read-only console of captured log lines (autoscroll).
    void DrawConsolePanel(const ConsoleBuffer& console);

    struct ViewportPanelResult
    {
        ViewportRect imageRect{};   // screen-space rect of the drawn image
        bool         hovered = false;
        bool         focused = false;
        uint32_t     desiredW = 0;  // content-region size (for OffscreenCanvas::Resize)
        uint32_t     desiredH = 0;
        bool         clicked = false;       // left-click landed inside the image this frame
        bool         altHeld  = false;      // alt modifier at click time (cycle stack)
        bool         ctrlHeld = false;      // ctrl modifier at click time (multi-select toggle)
        float        clickLocalX = 0.0f;    // viewport-local px of the click
        float        clickLocalY = 0.0f;
        // The dock node the Viewport currently lives in (0 = floating) --
        // where new editor documents dock as sibling tabs.
        unsigned int dockId = 0;
    };

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          bool& gizmoEnabled, Arcane::GizmoMode& mode,
                                          Arcane::GizmoSpace& space);

    // The Outliner (replaces the flat Hierarchy panel). Pure row data comes
    // from BuildOutlinerRows (EntityList.hpp, headless-tested); this shell
    // draws it and routes EVERY structural edit through ApplyRegistryMutation
    // over binding.snapshot/restore (Runtime::SnapshotRegistry/RestoreRegistry).
    // binding.editMode == false (Play running) disables structural edits --
    // the slice-2 resolution of RegistryStateCommand.hpp's native-state note.
    //
    // Named SceneEditBinding rather than OutlinerBinding since slice 4: the
    // Inspector's Add/Remove Component are structural edits too and share it.
    struct SceneEditBinding
    {
        Arcane::RegistryStateCommand::SnapshotFn snapshot;
        Arcane::RegistryStateCommand::RestoreFn  restore;
        bool editMode = true;    // false during Play: structural edits disabled
    };
    struct OutlinerState
    {
        char search[128] = {};
        std::unordered_set<std::uint64_t> collapsed;
        OutlinerSort sort;
        Astra::Entity renameTarget = Astra::Entity::Invalid();
        // std::string, not a fixed array: Identity::name is a std::string and
        // a 256-byte box silently truncated longer ones on the way in.
        std::string renameBuf;
        bool renameFocusPending = false;
        // A rename Edit::RenameWithUndo REFUSED because another transaction was
        // open (RenameResult::Deferred -- it will not join one, see EntityOps.hpp),
        // parked for the retry at the top of the next DrawOutlinerPanel. The
        // refusal mutates nothing, so parking the pair loses no edit. Invalid
        // means "no rename is parked"; the name rides along because the rename
        // box is gone by then.
        Astra::Entity pendingRename = Astra::Entity::Invalid();
        std::string pendingRenameName;
        Astra::Entity lastClicked = Astra::Entity::Invalid();
        double lastClickTime = 0.0;
        // Latched by the row menu's "Add Component..." and consumed at panel
        // scope one step later: ImGui cannot open a popup from inside another
        // popup's scope.
        bool addComponentPending = false;
    };
    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           OutlinerState& state);

    // Show the selected entity's components (via Registry::InspectEntity) and edit
    // reflected fields in place; unsupported types render read-only. Each field
    // edit gesture is bracketed into `undo` (Begin+SnapshotComponent on first
    // activation, Commit on release-after-edit, Cancel on a pure click) so every
    // Inspector edit becomes a Ctrl+Z/Y-undoable step -- but ONLY when
    // `binding.editMode` is true. While Play is running, it is false and the visitor's stack
    // pointer is left null, so the gesture bracketing fully no-ops (no Begin, no
    // Commit/Cancel): a play-time edit must not write against the live simulating
    // registry through the Edit-mode undo stack (Stop's Runtime::RestoreRegistry
    // swaps the registry back but does not touch the stack, so a stale entry here
    // would let a later Ctrl+Z overwrite the restored value with play-time bytes).
    // `binding` also carries the registry snapshot/restore seam, which is what
    // makes Add/Remove Component (structural, whole-registry memento) possible
    // from this panel -- the field-edit path above still uses the fine-grained
    // ComponentEditCommand gestures.
    // `project` (may be null) resolves Guid asset-ref fields to display names and
    // feeds the pick popup; null renders asset refs read-only-with-guid.
    //
    // Persistent Inspector state. A field-edit gesture opens its undo transaction
    // on widget activation in one frame and closes it on deactivation in a LATER
    // one, so the CommandStack ownership token (see CommandStack::Begin) has to
    // outlive the per-frame field visitor -- parking it here is what stops an
    // Inspector edit from committing or cancelling a concurrent gizmo drag's
    // transaction, and vice versa.
    struct InspectorState
    {
        Arcane::TransactionId gestureTxn = Arcane::TransactionId::None;
        // ImGui id of the widget that OPENED gestureTxn. The ownership guards
        // (EndGesture, CloseAbandonedGesture) compare against it, so it is
        // meaningful only while a gesture THIS panel opened is live; it is not
        // an "is one open" flag and must not be read as one. In particular a
        // gesture that JOINED a gizmo drag parks a real item id here alongside
        // gestureTxn == None, so a non-zero value does not imply an open
        // transaction of ours. gestureTxn is the authority on that.
        //
        // Why it is parked at all: a gesture closes on the widget's own
        // deactivation, which can only be observed while that widget is still
        // being SUBMITTED -- so a field that stops being drawn mid-gesture
        // would strand the transaction open, and with it every structural edit
        // (CanEditStructure is `!undo.InTransaction()`). The id lets the panel
        // see that ImGui's ActiveId has moved on and close the gesture itself.
        ImGuiID gestureItem = 0;
        // The string row's cancel reference, LATCHED AT ACTIVATION rather than
        // rebuilt per frame. ImGui's Escape restores the text it captured when
        // the box was activated (TextToRevertTo, imgui_widgets.cpp:4865-4866,
        // written back at :5300-5308), so the commit guard has to compare
        // against that same activation-time text. A per-frame seed read from
        // the live component diverges the moment anything changes the value
        // externally mid-edit (an Outliner rename committing a frame after the
        // Inspector box was activated on the same entity) -- Escape then
        // restored the OLD text, the guard saw a difference, and the
        // documented CANCEL path wrote an undo entry. Latched, the seed equals
        // TextToRevertTo by construction, so an Escape revert is always an
        // equality no-op.
        std::string stringEditSeed;
        // ImGui id of the row that latched stringEditSeed. Two string rows can
        // hand ActiveId over inside ONE frame (click into A, then click B drawn
        // ABOVE it: B activates and re-latches before A, submitted later,
        // reports its deactivation), which would have A committing against B's
        // seed. Same ownership-guard shape as gestureItem above.
        ImGuiID stringEditItem = 0;
        // Live search text. A fixed buffer rather than std::string because
        // ImGui::InputText writes into it directly; 128 is far past any
        // plausible field name. Nothing clears it, so a typed filter persists
        // across selection changes -- the Outliner's search behaves the same way.
        char searchBuffer[128] = {};
        // Width of the LABEL column, shared by every field grid in the panel.
        //
        // UE's Details panel has ONE draggable split for the whole panel, and
        // ImGui tables own their column widths individually with no
        // cross-table binding -- so this float is the authority instead: each
        // grid seeds its label column from it and adopts it back when the user
        // moves THAT grid's split (see BeginFieldGrid in EditorPanels.cpp).
        // Session-scoped by design: the grids pass
        // ImGuiTableFlags_NoSavedSettings so imgui.ini never becomes a second
        // authority that would fight this one on the next launch.
        //
        // 0 means "no width chosen yet"; the first grid drawn seeds it from
        // the panel's available width.
        float labelColWidth = 0.0f;
    };
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project, InspectorState& state);
}
