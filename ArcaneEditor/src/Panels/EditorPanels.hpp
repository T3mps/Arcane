#pragma once

#include "Scene/EditGesture.hpp"   // EditGesture::GestureState (InspectorState parks one)
#include "Panels/EntityList.hpp"
#include "Panels/InspectorFields.hpp"   // Arcane::Editor::QuatEulerView (InspectorState::quatEulerViews)
#include "Panels/PanelRegistry.hpp"   // PanelVisibility (BeginDockSpace's Window menu)
#include "Project/RecentProjects.hpp"   // RecentSelection (File -> Open Recent)
#include "Project/SceneRecents.hpp"   // SceneRecents::List (File -> Open Recent Scene)
#include "Viewport/ViewportInput.hpp"
#include "Viewport/ViewportSettings.hpp"   // ViewportToolState (ViewMode + ViewportSettings)
#include <Arcane/Edit/CommandStack.hpp>
#include <imgui.h>   // ImDrawList / ImVec2 (ViewportImageOverlayFn)
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Edit/RegistryStateCommand.hpp>
#include <Arcane/Guid.hpp>   // InspectorServices::mintSpriteForTexture
#include <Arcane/Util/FunctionRef.hpp>   // ApplyStructural's mutate callback
#include <cstdint>
#include <functional>
#include <glm/vec2.hpp>   // InspectorState::vectorProbe
#include <glm/vec4.hpp>   // InspectorState::colorPopupOriginal
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane { class RunLoop; class Runtime; class Project; class PluginHost; struct PluginVTable; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class ConsoleBuffer;
    class PlaySession;
    enum class PlayLaunchMode;   // full definition in PlayMode.hpp
    struct SelectionContext;
    class AssetPanelModel;   // full definition in AssetPanelModel.hpp (InspectorServices::assetModel)

    // Menu-bar requests the app resolves AFTER the frame's dockspace is drawn
    // (dialog launches happen at the call site, never inside the menu draw).
    struct MenuRequests
    {
        bool openProject = false;    // File -> Open Project      (file dialog)
        // A picked recent-project path. Empty = nothing picked this frame.
        // A path rather than a bool because a submenu carries the choice.
        std::string openRecentPath;
        // A picked recent-scene path (File -> Open Recent Scene). Empty =
        // nothing picked this frame. Routed through the scene-open dialog
        // slot (m_dialogs.sceneOpen) so it inherits the unsaved-scene
        // guard rather than re-earning it.
        std::string openRecentScenePath;
        // OUTPUT, not a request: the File menu is open this frame. The app uses
        // it to refresh the Open Recent cache on demand -- rebuilding it every
        // frame would re-read a file and stat every project for a menu almost
        // nobody has open.
        bool fileMenuOpen = false;
        // Assets -> Create -> <kind>...  A CreateAssetKind value
        // (Panels/CreateAssetDialog.hpp); -1 = nothing picked this frame.
        //
        // ASSET-MANAGER REDESIGN, PLAN 1 TASK 12: this ONE int replaces the
        // old `newMaterial`/`newMeshMaterial` bool pair, and with them the two
        // ShowSaveFileDialog launches they drove. Spec s7's invariant is why:
        // "no creation path may bypass CreateAssetRequest" -- so the menu, like
        // every other producer, raises a kind and nothing else, and
        // EditorApp::BeginCreateAsset is the one place a create dialog opens.
        int requestCreateKind = -1;
        // F4 plan 1 Task 11 (spec s8): Assets -> Create -> Mesh -> <primitive>
        // raises requestCreateKind = Mesh AND this MeshSource value (the app
        // copies it into CreateAssetRequest::prefillMeshSource). -1 = none.
        // Same pair AssetPanelActions carries for the panels' Create menus.
        int requestMeshSource = -1;
        // NO MENU RAISES THIS TODAY (the restructure dropped File -> Open
        // Material...; the Assets panel double-click is the open path). The
        // request + its dialog handler stay wired for the wiring pass.
        bool openMaterial = false;
        bool newScene = false;       // File -> New Scene
        bool openScene = false;      // File -> Open Scene...     (open-file dialog)
        bool saveScene = false;      // File -> Save Scene        (Save As when never saved)
        bool saveSceneAs = false;    // File -> Save Scene As...  (save dialog)
        bool rebuildModule = false;  // Build -> Rebuild Game Module (worker premake+msbuild)
        bool openIde = false;        // Build -> Open Visual Studio (IdeLaunch; generates the .slnx first if missing)
        bool resetLayout = false;   // Window -> Reset Layout (rebuild default dock layout, re-show all)
        bool selectAll = false;        // Edit -> Select All
        bool deselectAll = false;      // Edit -> Deselect All
        bool invertSelection = false;  // Edit -> Invert Selection
        bool renameSelected = false;   // Edit -> Rename (the Outliner's F2 code path)
        bool deleteSelected = false;   // Edit -> Delete (the Outliner's Del code path)
        bool cutSelection = false;         // Edit -> Cut
        bool copySelection = false;        // Edit -> Copy
        bool paste = false;                // Edit -> Paste
        bool duplicateSelection = false;   // Edit -> Duplicate
        bool saveAll = false;        // File -> Save All (scene + every dirty document)
        bool exitEditor = false;     // File -> Exit
        bool showInExplorer = false;   // Assets -> Show in Explorer (on the browser's tracked row)
        bool copyAssetPath = false;    // Assets -> Copy Path        (on the browser's tracked row)
        bool togglePhysicsOverlay = false;   // View -> Physics Overlay
#if !defined(ARCANE_DIST)
        // Build -> Diagnostics -> Crash GPU (diagnostics test). Dev-only, and
        // the only menu request whose SUCCESS is this process dying: it
        // dispatches Arcane::GpuFaultInjector and the device is expected to be
        // lost. See the item's own comment in EditorPanels.cpp.
        bool crashGpu = false;
#endif
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
    // `ideState` gates Build -> Open Visual Studio (IdeMenuState below):
    // greyed with a tooltip when no project is open or no Visual Studio
    // install was found, exactly UE's CanAccessSourceCode greying.
    // `panels` drives the Window menu's toggles; only Reset Layout goes
    // through `requests` (it must run at EndDockSpace's DockBuilder-safe
    // point).
    // `hasSelection` gates the Edit menu's selection-dependent items
    // (Rename/Delete, and Cut/Copy/Duplicate -- Paste stays always-enabled,
    // see its MenuItem call).
    // `hasAssetSelection` gates the Assets menu's Show in Explorer / Copy
    // Path (the Assets panel's last-clicked row -- AssetPanelModel::selected).
    // `sceneRecents` is the PER-PROJECT scene history (SceneRecents.hpp) that
    // drives File -> Open Recent Scene -- unlike `recents` above, which is the
    // Hub's shared, machine-wide project list.
    // Why Build -> Open Visual Studio is enabled or not this frame. The app
    // derives it (project open? devenv resolved?) and the menu only renders
    // it -- the tooltip wording lives with the item, the facts with the app.
    enum class IdeMenuState { Available, NoProject, NoVisualStudio };

    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing,
                        bool buildingModule, bool hasGameModule,
                        IdeMenuState ideState,
                        PanelVisibility& panels,
                        bool hasSelection,
                        bool hasAssetSelection,
                        bool physicsOverlayOn,
                        const RecentSelection* recents = nullptr,
                        const SceneRecents::List* sceneRecents = nullptr);

    // Emit the DockSpace() into the host window opened by BeginDockSpace and close it.
    // Everything drawn in between becomes a fixed (non-dockable, tab-less) strip above
    // the dockspace. `resetLayout` (Window -> Reset Layout) rebuilds the default dock
    // layout at this call's DockBuilder-safe point, same as the first-run path.
    void EndDockSpace(bool resetLayout = false);

    // Centered Play/Pause/Step transport, drawn as a FIXED STRIP into the current window
    // -- call between BeginDockSpace and EndDockSpace so it lands in the dockspace host.
    // Not its own window: no tab, cannot be docked or moved. Unity-style toggles (Play
    // tinted while playing, Pause tinted while paused + disabled outside Play, Step
    // disabled outside Play). Undo/Redo moved to the Edit menu; the gizmo tools moved to
    // the Viewport overlay. `host` is the hosted PluginHost (may be null): Play/Stop
    // route through its vtable's SaveState/LoadState so the plugin re-establishes its
    // native resources on restore, AND it is what attaches/detaches the embedded server
    // world in the EmbeddedServer topology -- one parameter, both jobs (Core-DLL split,
    // plan 1 Task 7; it used to be a bare `const PluginVTable*`).
    // Does NOT take a RunLoop&: play.Stop() ->
    // Runtime::RestoreRegistry destroys and replaces the RunLoop, so the loop is fetched
    // fresh from `runtime` AFTER Play/Stop handling. `logoTex` is the Arcane logo as an
    // ImGui texture id (the raw nri::Texture* as a uint64_t, matching DrawViewportPanel's
    // convention); drawn as a small mark at the far LEFT of the strip (Unity-style). 0 skips it.
    //
    // `mode` is the play-mode dropdown's persisted state, read AND written
    // here: the chevron button after Step opens a popup whose
    // rows set it directly. In Viewport mode the Play button behaves exactly as
    // before (play.Play/Stop). In SeparateWindow mode, clicking Play does NOT touch
    // `play` at all (fire-and-forget: nothing to Stop) -- instead this returns true
    // for that one frame, and the caller performs the actual ArcaneRuntime spawn
    // via EditorApp::DoLaunchStandalone, the same "panel reports, app performs"
    // split ViewportPanelResult's clicks already use. The project/dirty-scene
    // checks are NOT owned by that spawn step -- they live in the SceneSession
    // intent machine (SceneSession::Request, run by RunSceneAction before this
    // ever returns true); DoLaunchStandalone keeps only a defensive backstop.
    //
    // ListenServer/EmbeddedServer enter Play right here, like Viewport, differing
    // only in the PlayTopology handed to play.Play. SeparateServerProcess does
    // BOTH halves: it enters Play as a CLIENT world here AND sets
    // `launchServerRequested` for that one frame, which the caller turns into an
    // ArcaneServer.exe spawn (EditorApp::DoLaunchServer) -- the same "panel
    // reports, app performs" split as the return value above, in its own out
    // parameter because the two requests are independent and can never both be
    // true. It is always written (true or false) before this returns.
    [[nodiscard]] bool DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                                          Arcane::PluginHost* host,
                                          PlayLaunchMode& mode, bool& launchServerRequested,
                                          uint64_t logoTex = 0);

    // (The three asset panels are the REAL browser now --
    // AssetBrowserPanel/AssetGraphPanel/AssetStatusPanel, panel-split Task 7;
    // the placeholder stub retired in Slice 6, and Slice 6's OWN
    // DrawAssetBrowserPanel retired in the asset-manager arc's Task 15, which
    // is why that name was free for the window Task 7 shipped.)


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
    // `open` is forwarded to ImGui::Begin (the tab's X button; null = no X).
    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui, bool* open = nullptr);

    struct ViewportPanelResult
    {
        ViewportRect imageRect{};   // screen-space rect of the drawn image
        bool         hovered = false;
        bool         focused = false;
        // True on the ONE frame this window became the visible tab (see the
        // ImGui::IsWindowAppearing cite at the assignment site). Drives the
        // center-tab -> side-panel focus follow, NOT input routing.
        bool         appearing = false;
        uint32_t     desiredW = 0;  // content-region size (feeds the viewport vehicle's deferred resize)
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

    // User-initiated jump to a side panel (Focus in Graph, Reveal in Browser,
    // digest -> Status). SelectDockTab alone is not enough when the SOURCE
    // window shares the dock node: imgui.cpp:19611-19613 reapplies NavWindow
    // as the node's selected tab every frame, so a QueueFocus from a Browser
    // context menu flashes Graph then snaps back. Moving NavWindow to the
    // TARGET is what makes the tab stick -- the same NavWindow rule the
    // Inspector/Material auto-follow must NOT trip, because that path runs
    // after a center document's SetNextWindowFocus and would steal the
    // center tab. Auto-follow stays SelectDockTab; a click that means
    // "take me there" uses this.
    void FocusDockTab(const char* windowName);

    // Everything the Viewport's tool overlay reads and writes (F4 plan 1 T8).
    // References into EditorApp's state, so a click on the overlay edits the
    // host's member directly and the host reads the new value next frame:
    // the view-mode segments assign `viewMode` exactly as the Alt+G / Alt+J
    // keys do (EditorCamera::Resolve reads it), and the settings popup's
    // fovYDeg / speedScalar / settings edits are live the same way. All of it
    // persists through the [EditorViewport][Camera] ini block (Task 7's
    // handler, ViewportSettings.hpp).
    struct ViewportToolState
    {
        bool&                              gizmoEnabled;
        Arcane::GizmoMode&                 mode;
        Arcane::GizmoSpace&                space;
        Arcane::Editor::ViewMode&          viewMode;
        Arcane::Editor::ViewportSettings&  settings;
        float&                             fovYDeg;
        float&                             speedScalar;
    };

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    // showToolOverlay gates the top-right tool overlay (the 2D | Persp view
    // control, the view-settings gear, and the transform-tool buttons): the
    // host passes false in Play mode, where the game owns the viewport and
    // the edit tools (like the gizmo they drive) have no business on screen.
    // imageOverlay, when set, is called right after the image is drawn with
    // the Viewport window's draw list (clipped to the image) and the image's
    // screen origin -- the editor's FOREGROUND: the transform gizmo paints
    // here, over the finished frame, under the tool overlay's buttons
    // (Viewport/GizmoOverlay.hpp). Skipped when there is no image.
    using ViewportImageOverlayFn = std::function<void(ImDrawList& list, ImVec2 origin)>;
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          ViewportToolState& tools, bool showToolOverlay,
                                          const ViewportImageOverlayFn& imageOverlay = {});

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
        // F4 plan 1 Task 11 (spec s8): `Add 3D Object > <primitive>`, latched
        // by the row menu (parent = the row) or the panel-scope menu (parent =
        // Invalid -> SceneRoot, CreateEntityInScene's rule) and consumed by the
        // APP right after DrawOutlinerPanel returns (EditorApp::
        // ConsumeAddPrimitive) -- the "panel reports, app performs" split,
        // because the two things the spawn needs live on the app, not the
        // panel: the mesh asset (EditorApp::MintOrReusePrimitiveMesh touches
        // the project registry + disk, which no panel draw may) and the spawn
        // point (the editor camera's FocusPoint). A MeshSource value; -1 = none.
        int           addPrimitivePending = -1;
        Astra::Entity addPrimitiveParent  = Astra::Entity::Invalid();
    };

    // Promoted out of EditorPanels.cpp's anonymous namespace so the Edit
    // menu (EditorAppFrame.cpp) can drive the exact same code the Outliner's
    // F2/Del bindings use -- one implementation, two entry points, menu and
    // keybind cannot drift.
    //
    // `touched` names the entities the edit affects, for the Outliner's
    // unsaved asterisks (CommandStack::TouchedSinceState) -- read AFTER
    // mutate() runs, so creates can append their new ids from inside the
    // lambda (ApplyRegistryMutation's contract).
    bool ApplyStructural(Arcane::CommandStack& undo, const SceneEditBinding& b,
                         std::string label, Arcane::FunctionRef<bool()> mutate,
                         const std::vector<Astra::Entity>* touched = nullptr);

    // `current` is Identity::name RAW -- never Edit::DisplayName, which
    // substitutes "Entity <id>" for an empty name (EntityOps.cpp:49-55).
    // Seeding that fallback made a no-edit commit on an empty-named entity
    // write "Entity 7" into the component; seeding the raw (possibly empty)
    // name keeps Escape and no-edit commits true no-ops.
    void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current);

    void DeleteSelection(Astra::Registry& registry, SelectionContext& sel,
                         Arcane::CommandStack& undo, const SceneEditBinding& binding);

    // Edit-menu clipboard semantics, shared by the menu-bar consume
    // (EditorAppFrame.cpp), the keybinds, and the Outliner's context menus --
    // one implementation so the three routes cannot drift.
    // Copy returns whether anything reached the clipboard.
    bool CopySelectionToClipboard(Astra::Registry& registry, const SelectionContext& sel);
    // Cut = Copy + delete the captured subtrees as ONE undo step. Refuses
    // whole (and leaves the clipboard alone) when structural edits cannot
    // apply -- a refused cut must not clobber the clipboard.
    void CutSelection(Astra::Registry& registry, SelectionContext& sel,
                      Arcane::CommandStack& undo, const SceneEditBinding& binding);
    void PasteFromClipboard(Astra::Registry& registry, SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding);
    void DuplicateSelection(Astra::Registry& registry, SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding);

    // `savedStateId` is the scene's save baseline (SceneSession::SavedStateId)
    // -- the panel derives the per-entity unsaved asterisks from it via
    // CommandStack::TouchedSinceState. `open` is forwarded to ImGui::Begin
    // (the tab's X button; null = no X).
    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           OutlinerState& state, std::uint64_t savedStateId,
                           bool* open = nullptr);

    // App-level effect the Inspector panel triggers but does not own. UNLIKE
    // AssetPanelActions -- which only RETURNS a request and defers every
    // effect until AFTER the asset panels return ("Row actions the APP
    // resolves after the draw", AssetPanelCommon.hpp) -- this callback runs
    // its file IO + project-registry mutation SYNCHRONOUSLY, DURING
    // DrawInspectorPanel's own draw; there is no deferred step here. That is
    // safe because the Inspector draws AFTER all three asset panels every
    // frame (EditorApp::MainLoop: DrawEditorUi, which owns
    // DrawAssetBrowserPanel/DrawAssetGraphPanel/DrawAssetStatusPanel, runs
    // before DrawSelectionPanels, which owns DrawInspectorPanel) -- the asset
    // panels have already built and fully consumed their own per-frame entry
    // snapshot by the time this callback can run, so mutating the project's
    // asset registry here cannot invalidate anything an asset panel is still
    // iterating this frame. The one rule that DOES carry over
    // unchanged: no dialogs launch from inside a panel draw, on either path.
    //
    // Sprite-asset arc, Task 4: dropping a TEXTURE onto a sprite-typed
    // AssetRef field mints (or reuses) the wrapping .arcsprite; EditorApp
    // builds this ONCE (mintSpriteForTexture wraps
    // EditorApp::MintOrReuseSpriteForTexture) and passes it in by pointer every
    // frame, so the field visitor never needs to know about EditorApp itself.
    struct InspectorServices
    {
        std::function<Arcane::Guid(const Arcane::Guid&)> mintSpriteForTexture;

        // F2b Task 13: Guid -> an ImGui texture id (the raw nri::Texture*
        // through uintptr_t, ImGuiNri's convention -- 0 = unavailable) for the
        // Inspector's texture-asset preview. Resolves through the CHROME
        // context's texture cache via Assets::PixelsFor -- see
        // EditorApp::StageSpriteTables' own comment for why chrome, not the
        // viewport. Null callback (every headless test) degrades to "no
        // preview", same shape as a null mintSpriteForTexture.
        std::function<std::uint64_t(const Arcane::Guid&)> resolveTexturePreview;

        // Asset-manager arc, Task 14: the subkind-filtered material picker's
        // surface lookup. Points at EditorApp's OWN AssetPanelModel -- the
        // SAME cached, already-invalidation-correct surface answer the
        // Assets panel's Browse lens shows (AssetPanelModel::Find(guid)->
        // surface), not a fresh facade query -- so the Inspector's picker and
        // the Browse lens can never disagree about a material's surface. A
        // raw pointer, not a callable, because the model IS the answer (no
        // adaptation needed) and it is a stable member for the app's whole
        // lifetime -- set ONCE (EditorApp::StageSpriteTables, beside
        // mintSpriteForTexture above). Null for every caller that does not
        // wire InspectorServices at all (same convention as the other two
        // members): the picker then degrades to unfiltered, exactly like an
        // unrecognised owning component.
        const Arcane::Editor::AssetPanelModel* assetModel = nullptr;
    };

    // Asset-manager redesign, Plan 1 Task 7: the Assets panel's thumbnail
    // resolver seam. Same convention as InspectorServices::resolveTexturePreview
    // above (Guid -> an ImGui texture id via the CHROME context's texture
    // cache, 0 = unavailable -- the caller falls back to the kind icon), kept
    // as its own struct rather than folded into InspectorServices because the
    // consumer is a different panel (Tasks 9-11's Browse lens, not the
    // Inspector). Textures resolve directly; sprites resolve through their
    // referenced texture; materials route through Task 8's
    // MaterialPreviewHarvester (0 until then); everything else is 0. Task 9's
    // AssetPanelServices consumes this exact callable.
    struct AssetServices
    {
        std::function<std::uint64_t(const Arcane::Guid&)> resolveAssetThumb;
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
        // Per-field Euler-view cache for glm::quat rows (F1 Task 2): the
        // triple currently on screen for a Quat field, kept across frames so
        // "Euler is a VIEW, the quaternion is the STORAGE" holds -- see
        // Arcane::Editor::QuatEulerView's own comment (InspectorFields.hpp).
        // Lives HERE for the same reason colorPopupOriginal does (the visitor
        // is rebuilt every frame and cannot itself carry state), but UNLIKE
        // the colour popup this is not exclusive -- more than one Quat row
        // can be on screen in the same frame (multiple components, or a
        // future second glm::quat field) -- so it is keyed per field rather
        // than a single shared slot. The key combines the owning component's
        // descriptor hash with the field's own nameHash (see InspectorView.
        // cpp); only the PRIMARY entity's rotation is ever cached, matching
        // every other field kind's single-selection row, so switching the
        // primary entity is just another external change SyncQuatEulerView
        // already handles by re-deriving.
        std::unordered_map<std::uint64_t, Arcane::Editor::QuatEulerView> quatEulerViews;
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

        // TEST SEAM (2D physics wiring Plan 2, FieldKind::Vector). When
        // non-null, the vector editor records the screen-space CENTRE of every
        // list control it draws this frame -- "<field>.add",
        // "<field>[i].remove" / ".up" / ".down", and each element field row as
        // "<field>[i].<elementField>" -- so a device-less test can aim
        // io.AddMousePosEvent at them and click through the REAL ImGui path
        // (EditorInspectorVectorTest.cpp). ImGui keeps no item-rect registry a
        // test could read instead. Production never sets it: nullptr, one
        // branch per control. Same "exposed on purpose so the test can reach
        // it" shape as AssetGraphPanelState::graphCanvas. Nothing here clears
        // it -- the test owns the map and clears it per frame.
        std::unordered_map<std::string, glm::vec2>* vectorProbe = nullptr;
    };
    // `open` is forwarded to ImGui::Begin (the tab's X button; null = no X).
    // `selectedAsset` (F2b Task 13): the Assets panel's last-clicked row
    // (AssetPanelModel::selected). Consulted ONLY when there is no entity
    // selection -- an entity selection always wins, matching every other
    // "two things could occupy this panel" tie-break in the editor (e.g. the
    // Material panel's own free function below routes the ACTIVE DOCUMENT,
    // never a browser selection). A nil guid (the default) behaves exactly
    // like the pre-Task-13 signature: "No selection" when nothing is
    // entity-selected either.
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project, InspectorState& state,
                            const InspectorServices* services = nullptr,
                            bool* open = nullptr,
                            const Arcane::Guid& selectedAsset = Arcane::Guid{});
}
