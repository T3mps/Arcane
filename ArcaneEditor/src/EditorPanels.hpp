#pragma once

#include "ViewportInput.hpp"
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <cstdint>

namespace Arcane { class RunLoop; class Runtime; class Project; struct PluginVTable; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class ConsoleBuffer;
    class PlaySession;
    struct SelectionContext;

    // Open the full-viewport dockspace host window + the editor menu bar and LEAVE IT
    // OPEN (call once per frame right after ImGui BeginFrame). Draw the fixed toolbar
    // strip (DrawSimTimeToolbar) into it, then close it with EndDockSpace(); dockable
    // panels are drawn AFTER EndDockSpace. `undo` drives the Edit menu's Undo/Redo
    // (same CommandStack as the Ctrl+Z / Ctrl+Y shortcuts handled in the app input loop).
    void BeginDockSpace(Arcane::CommandStack& undo, bool& openProjectRequested);

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
    // fresh from `runtime` AFTER Play/Stop handling.
    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin);

    // Placeholder asset-browser panel (dummy for now); docks as a tab before Console.
    // Shows the open project's name + root when one is open (see EditorApp::Init),
    // else "No project open" (the legacy data/-next-to-exe boot, unchanged).
    void DrawAssetsPanel(const Arcane::Project* project);

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
        float        clickLocalX = 0.0f;    // viewport-local px of the click
        float        clickLocalY = 0.0f;
    };

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          bool& gizmoEnabled, Arcane::GizmoMode& mode,
                                          Arcane::GizmoSpace& space);

    // List every live entity; clicking a row selects it. Labels rows by id
    // ("Entity <id> (v<version>)") since no Name component exists yet.
    void DrawHierarchyPanel(Astra::Registry& registry, SelectionContext& sel);

    // Show the selected entity's components (via Registry::InspectEntity) and edit
    // reflected fields in place; unsupported types render read-only. Each field
    // edit gesture is bracketed into `undo` (Begin+SnapshotComponent on first
    // activation, Commit on release-after-edit, Cancel on a pure click) so every
    // Inspector edit becomes a Ctrl+Z/Y-undoable step -- but ONLY when `editMode`
    // is true. While Play is running, `editMode` is false and the visitor's stack
    // pointer is left null, so the gesture bracketing fully no-ops (no Begin, no
    // Commit/Cancel): a play-time edit must not write against the live simulating
    // registry through the Edit-mode undo stack (Stop's Runtime::RestoreRegistry
    // swaps the registry back but does not touch the stack, so a stale entry here
    // would let a later Ctrl+Z overwrite the restored value with play-time bytes).
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, bool editMode);
}
