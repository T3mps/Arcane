#pragma once

#include "ViewportInput.hpp"
#include <Arcane/Edit/CommandStack.hpp>
#include <cstdint>

namespace Arcane { class RunLoop; class Runtime; struct PluginVTable; }
namespace Astra { class Registry; }

namespace Grimoire
{
    class ConsoleBuffer;
    class PlaySession;
    struct SelectionContext;

    // Host a full-viewport dockspace (call once per frame between ImGui BeginFrame
    // and the panel Begin/End calls). Enables initial docking of child windows.
    void BeginDockSpace();

    // Play/Stop (play-in-editor: snapshot on Play, restore on Stop) + Pause/Step
    // buttons + a time-scale slider, driving the RunLoop, plus Undo/Redo buttons
    // over `undo` (enabled from CanUndo/CanRedo, tooltip shows the label).
    // `plugin` is the hosted plugin's vtable (may be null): Play/Stop route
    // through its SaveState/LoadState so the plugin re-establishes its native
    // resources on restore. Entering Play clears `undo`'s history (play-time
    // mutation is discarded on Stop, so it must not be undoable from Edit
    // afterwards). Does NOT take a RunLoop& parameter: play.Stop() ->
    // Runtime::RestoreRegistry destroys and replaces the RunLoop, so the loop
    // is fetched fresh from `runtime` AFTER the Play/Stop handling to avoid a
    // dangling reference.
    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin,
                            Arcane::CommandStack& undo);

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
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH);

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
