#pragma once

#include "EditGesture.hpp"   // EditGesture::GestureState (InspectorState parks one)
#include "EntityList.hpp"
#include "RecentProjects.hpp"   // RecentSelection (File -> Open Recent)
#include "ViewportInput.hpp"
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Guid.hpp>   // InspectorServices::mintSpriteForTexture
#include <cstdint>
#include <functional>
#include <glm/vec4.hpp>   // InspectorState::colorPopupOriginal
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane { class RunLoop; class Runtime; class Project; struct PluginVTable; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class ConsoleBuffer;
    class PlaySession;
    enum class PlayLaunchMode;   // full definition in PlayMode.hpp
    struct SelectionContext;

    // Menu-bar requests the app resolves AFTER the frame's dockspace is drawn
    // (dialog launches happen at the call site, never inside the menu draw).
    struct MenuRequests
    {
        bool openProject = false;    // File -> Open Project      (file dialog)
        // A picked recent-project path. Empty = nothing picked this frame.
        // A path rather than a bool because a submenu carries the choice.
        // NO MENU RAISES THIS TODAY: the 2026-08-10 menu restructure parked
        // the project-recents submenu (File shows a greyed "Open Recent
        // Scene" placeholder instead); the consumer plumbing is kept intact
        // for the wiring pass.
        std::string openRecentPath;
        // OUTPUT, not a request: the File menu is open this frame. The app uses
        // it to refresh the Open Recent cache on demand -- rebuilding it every
        // frame would re-read a file and stat every project for a menu almost
        // nobody has open.
        bool fileMenuOpen = false;
        bool newMaterial = false;    // Assets -> Create -> Material... (save dialog; graph-owned)
        // NO MENU RAISES THIS TODAY (the restructure dropped File -> Open
        // Material...; the Assets panel double-click is the open path). The
        // request + its dialog handler stay wired for the wiring pass.
        bool openMaterial = false;
        bool newScene = false;       // File -> New Scene
        bool openScene = false;      // File -> Open Scene...     (open-file dialog)
        bool saveScene = false;      // File -> Save Scene        (Save As when never saved)
        bool saveSceneAs = false;    // File -> Save Scene As...  (save dialog)
        bool rebuildModule = false;  // Build -> Rebuild Game Module (worker premake+msbuild)
    };

    // Open the full-viewport dockspace host window + the editor menu bar and LEAVE IT
    // OPEN (call once per frame right after ImGui BeginFrame). Draw the fixed toolbar
    // strip (DrawSimTimeToolbar) into it, then close it with EndDockSpace(); dockable
    // panels are drawn AFTER EndDockSpace. `undo` drives the Edit menu's Undo/Redo
    // (same CommandStack as the Ctrl+Z / Ctrl+Y shortcuts handled in the app input loop).
    // Menu clicks land in `requests`; the caller launches the dialogs.
    // `sceneDirty` puts the * on Save Scene; `playing` greys both Save items out.
    // `buildingModule`/`hasGameModule` gate Build -> Rebuild Game Module
    // (greyed while playing -- the UE model, see the item's own comment --
    // while a build is already running, and when the open project declares no
    // gameModule at all).
    // `recents` is currently PARKED (the 2026-08-10 restructure removed the
    // project-recents submenu; see MenuRequests::openRecentPath) -- still
    // passed so the wiring pass can resurface it without a signature change.
    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing,
                        bool buildingModule, bool hasGameModule,
                        const RecentSelection* recents = nullptr);

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
    //
    // `mode` is the play-mode dropdown's persisted state (Task 6, runtime-host-fold
    // arc), read AND written here: the chevron button after Step opens a popup whose
    // rows set it directly. In Viewport mode the Play button behaves exactly as
    // before (play.Play/Stop). In SeparateWindow mode, clicking Play does NOT touch
    // `play` at all (fire-and-forget: nothing to Stop) -- instead this returns true
    // for that one frame, and the caller (EditorApp::LaunchStandalone) owns the
    // project/dirty-scene checks and the actual ArcaneRuntime spawn, the same
    // "panel reports, app performs" split ViewportPanelResult's clicks already use.
    [[nodiscard]] bool DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                                          const Arcane::PluginVTable* plugin,
                                          PlayLaunchMode& mode, uint64_t logoTex = 0);

    // (The Assets panel is the REAL browser now -- AssetBrowser.hpp's
    // DrawAssetBrowserPanel; the placeholder stub retired in Slice 6.)


    // Console panel UI state. Owned by EditorApp so it survives the frame; the
    // panel mutates it in place (same shape as the viewport's gizmo toggles).
    struct ConsoleUiState
    {
        bool showInfo    = true;
        bool showWarning = true;
        bool showError   = true;
        bool collapse    = false;
        bool autoScroll  = true;
        bool wrap        = true;
        char search[128] = {};
        int  lineCap     = 512;
        // Copy button's "Copied" feedback: the ImGui::GetTime() deadline the
        // swapped label holds until. A plain deadline the draw compares each
        // frame -- no timer, no animation state; 0 (any past time) = idle.
        double copyFlashUntil = 0.0;
    };

    // Scrolling console of captured log lines: severity filters, text search,
    // collapse-identical, wrap toggle, Clear/Copy. Autoscroll pins to bottom.
    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui);

    struct ViewportPanelResult
    {
        ViewportRect imageRect{};   // screen-space rect of the drawn image
        bool         hovered = false;
        bool         focused = false;
        // True on the ONE frame this window became the visible tab (see the
        // ImGui::IsWindowAppearing cite at the assignment site). Drives the
        // center-tab -> side-panel focus follow, NOT input routing.
        bool         appearing = false;
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

    // Make `windowName` the VISIBLE TAB of whatever dock node it lives in,
    // WITHOUT moving keyboard/input focus. This is the side-panel switch:
    // ImGui::SetWindowFocus would also move g.NavWindow, and because the dock
    // tab bar applies NavWindow back as its node's selection every frame
    // (imgui.cpp:19611-19613), one stray focus call on a SIDE panel is what
    // stops a freshly-opened CENTER document from keeping the center tab.
    // Focus is for the thing you type into; tab selection is for the thing you
    // look at, and they are not the same request.
    //
    // No-op when the window does not exist yet, is not docked, or is alone in
    // its node (no tab bar) -- in every one of those there is no tab to select.
    void SelectDockTab(const char* windowName);

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    // showToolOverlay gates the top-right transform-tool buttons: the host passes
    // false in Play mode, where the game owns the viewport and the edit tools
    // (like the gizmo they drive) have no business on screen.
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          bool& gizmoEnabled, Arcane::GizmoMode& mode,
                                          Arcane::GizmoSpace& space, bool showToolOverlay);

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

    // App-level effect the Inspector panel triggers but does not own. UNLIKE
    // AssetBrowserActions -- which only RETURNS a request and defers every
    // effect until AFTER DrawAssetBrowserPanel returns ("Row actions the APP
    // resolves after the draw", AssetBrowser.hpp) -- this callback runs its
    // file IO + project-registry mutation SYNCHRONOUSLY, DURING
    // DrawInspectorPanel's own draw; there is no deferred step here. That is
    // safe because the Inspector draws AFTER the Asset Browser every frame
    // (EditorApp::MainLoop: DrawEditorUi, which owns DrawAssetBrowserPanel,
    // runs before DrawSelectionPanels, which owns DrawInspectorPanel) -- the
    // Browser has already built and fully consumed its own per-frame entry
    // snapshot by the time this callback can run, so mutating the project's
    // asset registry here cannot invalidate anything the Browser is still
    // iterating this frame. The one rule that DOES carry over unchanged: no
    // dialogs launch from inside a panel draw, on either path.
    //
    // Sprite-asset arc, Task 4: dropping a TEXTURE onto a sprite-typed
    // AssetRef field mints (or reuses) the wrapping .arcsprite; EditorApp
    // builds this ONCE (mintSpriteForTexture wraps
    // EditorApp::MintOrReuseSpriteForTexture) and passes it in by pointer every
    // frame, so the field visitor never needs to know about EditorApp itself.
    struct InspectorServices
    {
        std::function<Arcane::Guid(const Arcane::Guid&)> mintSpriteForTexture;
    };

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
        // Ownership slots (transaction token + owning item id), the
        // builder-style pending commit the Inspector never uses, and the string
        // row's activation-time cancel seed -- every member documented on
        // EditGesture::GestureState, which owns the behaviours that read them.
        EditGesture::GestureState gesture;
        // The colour a picker popup was opened on, for its Old/New pair. Lives HERE
        // rather than on the Inspector's per-field visitor because that visitor is
        // rebuilt every frame (it holds only a pointer to `gesture` above), so a
        // member there would reset each frame and Old would track New. One slot is
        // enough: only one colour popup can be open at a time.
        glm::vec4 colorPopupOriginal{ 1.0f, 1.0f, 1.0f, 1.0f };
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
        // moves THAT grid's split (this float is what the panel hands each
        // Arcane::Editor::FieldGrid -- see EditorWidgets.cpp's BeginFieldGrid,
        // which FieldGrid is the only public way to reach).
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
                            const Arcane::Project* project, InspectorState& state,
                            const InspectorServices* services = nullptr);
}
