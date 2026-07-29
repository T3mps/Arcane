#pragma once
// EditorApp: the editor application. Constructed in main from a HostConfig
// (shared with ArcaneRuntime); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors ArcaneRuntime.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include "AssetBrowser.hpp"
#include "ConsoleBuffer.hpp"
#include "DocumentHost.hpp"
#include "EditorCamera.hpp"
#include "EditorPanels.hpp"
#include "PlayMode.hpp"
#include "SceneSession.hpp"
#include "SelectionContext.hpp"
#include "ShaderEditorDocument.hpp"
#include "SpriteCache.hpp"
#include "ViewportInput.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/PickBuffer.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/SelectionOutline.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Render/SpriteMaterialCache.hpp>

#include <spdlog/sinks/callback_sink.h>

namespace Astra { class TypeContext; }
namespace Arcane { struct InputSnapshot; }   // by-reference phase parameters only

namespace Arcane::Editor
{
    class EditorApp
    {
    public:
        explicit EditorApp(HostConfig cfg);
        int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code

        // Raise File -> Open Project on the FIRST frame. Set by main() for a bare
        // interactive launch (no --project, no --plugin): the editor supports the
        // project-less state, and this is its cold-start path into the picker.
        void RaiseOpenProjectOnStart() noexcept { m_raiseOpenProjectOnStart = true; }

    private:
        bool Init();
        void MainLoop();
        void Shutdown();
        void InstallConsoleSink();   // attach a callback sink on Arcane::Log::Engine() -> m_console

        // ---- Frame loop (EditorAppFrame.cpp) --------------------------------
        // MainLoop is a straight sequence of the phase methods below, called in
        // exactly the order the statements inside them ran in when this was one
        // 1100-line function. That order is LOAD-BEARING and has NO automated
        // coverage -- EditorApp*.cpp is not compiled into ArcaneTests, so a green
        // suite proves nothing here. Each phase's definition carries the ordering
        // constraint it participates in; do not reorder the calls in MainLoop.

        // What PumpFrameEvents decided the rest of the frame should do: the
        // original block's fallthrough / `continue` / `break` respectively.
        enum class FramePump { Continue, SkipFrame, Exit };

        // Frame-loop state that outlives ONE frame -- these were MainLoop's
        // locals declared OUTSIDE the while loop, and MainLoop still owns the
        // single instance. Deliberately not EditorApp members: the lifetime is
        // exactly the MainLoop call, as before.
        struct LoopState
        {
            std::chrono::steady_clock::time_point simPrev{};
            std::chrono::steady_clock::time_point lastFrameTime{};
            bool running = true;
            // The answer the confirm modal resolved, carried from the ImGui pass of one
            // frame to the top of the next. The modal cannot perform the action where the
            // button is clicked: OpenProject tears down the plugin and closes documents
            // whose textures the frame's already-built draw lists still reference, and
            // ImGui replays those lists at Render time.
            Arcane::Editor::SceneSession::PendingRequest sceneAction;
        };

        // Values one phase computes and a LATER phase of the SAME frame reads.
        // These were locals declared at the top of the while body; MainLoop
        // declares one instance per iteration, so they are as fresh as before.
        struct FrameState
        {
            // Set inside the input phase once inViewport + the game context's
            // last-frame WantCaptureMouse are known; outlives that phase
            // so the click-pick further down this same frame can also honor it.
            bool gameUiClaims = false;
            // File-menu scene shortcuts, raised in the input phase and folded
            // into this frame's MenuRequests at the menu-request site (the same shape
            // m_raiseOpenProjectOnStart uses) so the keybind and the menu item cannot
            // drift apart.
            bool scNewScene = false, scOpenScene = false, scSaveScene = false;
            // This frame's Viewport panel result, read by the click-pick phase.
            Arcane::Editor::ViewportPanelResult vp{};
        };

        FramePump PumpFrameEvents();
        void RunSceneAction(const Arcane::Editor::SceneSession::PendingRequest& req,
                            LoopState& ls);
        void ConsumeDeferredSceneAction(LoopState& ls);
        void ConsumeSceneDialogResults(LoopState& ls);
        void ConsumeProjectDialogResult();
        void ConsumeMaterialDialogResults();
        void FrameInput(LoopState& ls, FrameState& fs);
        void HandleUndoRedoAndSceneShortcuts(const Arcane::InputSnapshot& snap, FrameState& fs);
        void HandleGizmoModeKeys(const Arcane::InputSnapshot& snap);
        void UpdateEditorCamera(const Arcane::InputSnapshot& snap, bool inViewport,
                                float lx, float ly);
        void UpdateGizmoInteraction(const Arcane::InputSnapshot& snap, bool inViewport,
                                    float lx, float ly, bool gameUiClaims);
        void AdvanceSim(LoopState& ls);
        void ApplyPendingViewportResize();
        void UpdateScenePostChain();
        void RenderSceneToViewport();
        void CompositeGameUi();
        void RenderSelectionOutline();
        void PumpShaderEditor();
        void DrawEditorUi(LoopState& ls, const FrameState& fs);
        void DrawModals(LoopState& ls);
        void DrawViewportPanelPhase(FrameState& fs);
        // Center-tab -> side-panel focus follow. Reads the one-frame
        // "became visible" edges the Viewport panel and the material documents
        // published THIS frame, and focuses the matching tab in their dock
        // node. Must run after both have drawn.
        void SyncCenterTabFocus(const FrameState& fs);
        void HandleViewportPick(const FrameState& fs);
        void DrawSelectionPanels();
        bool PresentFrame();
        void EndFrame(LoopState& ls);

        HostConfig                        m_config;
        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST

        // The hosted plugin's OWN ImGui context, rendered INTO the viewport's
        // output texture (Unity/Unreal "game view") so the plugin's debug HUD
        // lives inside the Viewport panel, never over the editor chrome. Created
        // in Init after the editor ImGui layer is up; the plugin is pointed at it
        // via Runtime::SetImGui (in place of the editor context). Declared after
        // m_gpu (destructs BEFORE the device -- it holds NVRHI handles built from
        // m_gpu->Device()) and before m_runtime/m_plugin (destructs AFTER the
        // plugin -- the plugin holds this ctx via SetImGui and may touch ImGui
        // during Unload/Shutdown, so this must outlive it). Its destructor
        // restores the editor context, so ~GpuContext's ImGuiLayer teardown stays
        // valid. See CompositeGameUi.
        std::unique_ptr<Arcane::OffscreenImGuiLayer> m_gameImgui;

        Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
        std::optional<Arcane::Runtime>    m_runtime;                // destructs before m_gpu
        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;
        ConsoleBuffer                     m_console{512};
        // Handle to the callback sink pushed onto Arcane::Log::Engine() in
        // InstallConsoleSink(); erased at the top of Shutdown() so the sink cannot
        // fire (and Push into a destroyed m_console) during member teardown -- e.g.
        // ~GpuContext's Vulkan device destruction logs validation messages.
        std::shared_ptr<spdlog::sinks::callback_sink_mt> m_consoleSink;

        // Play-in-editor (Task 8): Edit|Play state machine. Play() snapshots the
        // registry + unpauses the RunLoop; Stop() restores the snapshot + re-pauses.
        // Arcane Editor boots in Edit (see Init: the RunLoop is paused right after the
        // plugin loads).
        Arcane::Editor::PlaySession m_play;

        // Ordered multi-select source of truth (set + primary); slice-2 consumers
        // operate on Primary(), shared by the Hierarchy panel (and, later, the
        // Inspector + viewport pick -- see SelectionContext.hpp).
        Arcane::Editor::SelectionContext m_selection;

        // Which .arcscene is being edited, whether it has unsaved changes, and the
        // one-at-a-time unsaved-changes confirm machine (SceneSession.hpp). Pure
        // state: every effect below it (dialogs, file IO, ResetRegistry) is
        // performed here.
        Arcane::Editor::SceneSession m_scene;

        // Outliner panel state + structural-edit binding (slice 2)
        Arcane::Editor::OutlinerState   m_outliner;
        Arcane::Editor::SceneEditBinding m_editBinding;
        // Inspector panel state: holds the field-edit gesture's CommandStack
        // ownership token across the frames the gesture spans (see InspectorState).
        Arcane::Editor::InspectorState  m_inspector;
        // Sprite-asset arc, Task 4: built ONCE in Init (mintSpriteForTexture
        // wraps MintOrReuseSpriteForTexture) and handed to DrawInspectorPanel
        // every frame, so the field visitor's texture-drop auto-mint branch
        // never needs to know about EditorApp itself.
        Arcane::Editor::InspectorServices m_inspectorServices;

        // Editor undo/redo history. Deliberately NOT cleared on Play: Stop restores
        // the pre-Play registry, so the edits behind these entries are still on
        // screen and must stay undoable -- and because SceneSession reads this
        // stack's StateId as the SOLE input to the scene's dirty flag
        // (SceneSession.hpp), clearing it would silently report an unsaved scene as
        // clean. It is cleared only where no entity handle in it could survive:
        // ClearSceneReferences, ahead of a registry swap. Constructed in
        // Init once the runtime's registry exists; optional so it can be built
        // after m_runtime. Declared AFTER m_runtime/m_plugin so it destructs
        // BEFORE them -- its resolver lambda captures `&*m_runtime` (a raw
        // Runtime*, dereferenced fresh each call), so it must not outlive it.
        std::optional<Arcane::CommandStack> m_undo;

        // Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y edge-tracking for the undo/redo keybinds
        // (InputSnapshot only reports held-state, not rising-edge; tracked here
        // across frames -- see HandleUndoRedoAndSceneShortcuts).
        bool m_prevUndoKeyDown = false;
        bool m_prevRedoKeyDown = false;

        // Transform-gizmo state (Edit-mode; drives the Viewport handles). Mode/
        // space persist across frames and selection changes; m_gizmoHovered is
        // recomputed every frame the gizmo is interactable (cleared otherwise) so
        // Draw's highlight always matches this frame's cursor. m_gizmoDrag spans
        // the mouse-down..mouse-up gesture; `start` is the pre-drag GizmoTransform
        // so ApplyDrag recomputes from origin each frame (no accumulation drift).
        // Transform is parent-local, but every GizmoTransform stored here is WORLD
        // space (Unreal parity) -- EditorApp converts through Edit::WorldMatrix on
        // read and Edit::ParentWorldMatrix's inverse on write-back.
        Arcane::GizmoMode  m_gizmoMode    = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace m_gizmoSpace   = Arcane::GizmoSpace::World;
        bool               m_gizmoEnabled = false;  // false = Select tool (click-to-pick, no gizmo)
        Arcane::GizmoAxis  m_gizmoHovered = Arcane::GizmoAxis::None;
        struct GizmoDrag
        {
            bool                   active = false;
            Arcane::GizmoAxis      axis   = Arcane::GizmoAxis::None;
            Arcane::GizmoTransform start;                    // the PRIMARY's pre-drag WORLD pose (gizmo anchor)
            glm::vec2              mouseStartScreen{0.0f, 0.0f};
            // Every selection ROOT carrying a Transform, with its pre-drag WORLD
            // pose. Rebuilt on press. Roots only: a selected child already rides
            // its selected parent through WorldTransform propagation.
            std::vector<std::pair<Astra::Entity, Arcane::GizmoTransform>> targets;
            // Ownership token for the drag's undo transaction, minted on press
            // and spent on release (CommandStack::Begin). Parked here because the
            // gesture spans frames and the Inspector shares the same stack: only
            // the token holder may Commit/Cancel, so an Inspector edit landing in
            // the same frame can no longer close this drag out from under it.
            Arcane::TransactionId txn = Arcane::TransactionId::None;
        } m_gizmoDrag;

        // W/E/R mode-key edge-tracking (same pattern as m_prevUndoKeyDown/
        // m_prevRedoKeyDown above).
        bool m_prevKeyW = false;
        bool m_prevKeyE = false;
        bool m_prevKeyR = false;
        bool m_prevKeyQ = false;
        // Ctrl+N / Ctrl+O / Ctrl+S edge-tracking for the File-menu scene shortcuts
        // (same pattern as the undo/redo keys above).
        bool m_prevKeyN = false;
        bool m_prevKeyO = false;
        bool m_prevKeyS = false;
        // Left-mouse edge-tracking, shared by the gizmo press/release detection.
        bool m_prevLmbDown = false;

        // The EDITOR's viewport camera (Edit mode). Runtime::SetCamera is the
        // PLUGIN's seam, so a project whose game module never calls it would be
        // stuck at the identity transform -- offset (0,0), zoom 1, i.e. 1 px per
        // metre. EditorApp drives this from viewport input and pushes it into
        // the Runtime in Edit mode only; in Play the plugin's camera wins.
        // See EditorCamera.hpp for the transform convention.
        Arcane::Editor::EditorCamera m_camera;
        // RMB-drag pan gesture. Starts only inside the viewport but keeps
        // tracking once started (same rule as the gizmo drag), so a pan does not
        // stall the moment the cursor crosses the panel edge. The cursor is
        // kept in WINDOW px: the pan only needs the frame-to-frame DELTA, which
        // is identical in window and viewport-local space (they differ by a
        // translation), and window px are written every frame regardless of
        // whether the viewport is active.
        bool      m_camPanning = false;
        glm::vec2 m_camPanLastMouse{0.0f, 0.0f};
        bool      m_prevRmbDown = false;
        // F (frame selection) / Home (frame scene) edge-tracking, same pattern
        // as the undo/redo and gizmo mode keys above.
        bool m_prevKeyF = false;
        bool m_prevKeyHome = false;
        // Point the editor camera at the selection (selectionOnly) or at the
        // whole scene. A no-op when there is nothing framable, so the user's
        // view is never thrown away by an F press that had no target.
        void FrameCamera(bool selectionOnly);
        void FrameSceneIfPending();
        // Set whenever a scene becomes the current one, consumed on the first frame
        // the viewport has a real size. See FrameSceneIfPending for why it cannot
        // be immediate.
        bool m_frameOnSceneOpen = false;
        // Set for the remainder of THIS frame when a gizmo drag starts or ends,
        // so the click-pick phase (later in the frame) does not also treat the
        // same click as a selection change. Reset at the top of FrameInput
        // each frame.
        bool m_gizmoCapturedClick = false;

        // Scene-in-a-panel viewport: the same canvas->batcher->tonemap path ArcaneRuntime
        // drives the backbuffer with, rendered into a panel texture instead. Resized
        // to the Viewport panel's content region each frame; input into the plugin
        // is gated on m_viewportActive and remapped through m_viewportRect (see
        // ViewportInput.hpp + FrameInput).
        std::unique_ptr<Arcane::OffscreenCanvas> m_viewport;
        Arcane::Editor::ViewportRect                   m_viewportRect{};
        bool                                     m_viewportActive = false;

        // Viewport-local input snapshot for the game ImGui pass, captured inside
        // FrameInput (whose locals are out of scope at the render
        // site) and read where the game UI is composited into the viewport. Only
        // the few values the game context needs are hoisted -- the input phase's
        // scope is deliberately NOT widened.
        glm::vec2 m_lastViewportMouse{0.0f, 0.0f};   // viewport-local cursor px
        std::uint8_t m_lastMouseButtons = 0;         // raw snap.mouseButtons (LMB=bit0)
        float     m_lastWheel   = 0.0f;              // raw snap.wheelY
        bool      m_lastInViewport = false;          // cursor over the viewport this frame
        double    m_lastFrameDt = 0.0;               // per-frame dt (seconds)

        // GPU hit-proxy picker, a sibling of m_viewport: created and resized at the
        // same size, it renders each pickable entity's silhouette into an R32_UINT
        // id buffer and reads back the pixel under a viewport click to select the
        // entity there (sprites + physics colliders; front-most wins). Replaces the
        // CPU sprite-OBB PickEntitiesAt. See PickBuffer.hpp.
        std::unique_ptr<Arcane::PickBuffer>       m_pick;

        // Per-frame selection + hover outline (Edit-mode only), a sibling of m_pick:
        // created and resized at the same viewport size, it edge-detects m_pick's
        // (supersampled) id buffer into the viewport's post-tonemap output texture
        // (amber selected, cyan hovered). See RenderSelectionOutline, right after
        // RenderSceneToViewport -- the same slot the Play-only game-imgui overlay
        // pass uses (the two are mutually exclusive by mode).
        std::unique_ptr<Arcane::SelectionOutline> m_outline;

        // Shader-editor services + open documents (Slice 5). The compiler is the
        // app-shared compile service (documents Submit through it; PumpShaderEditor
        // Polls/Drains it once per frame and routes results to documents -- the
        // drain site is the ONE place compile results become NVRHI shaders).
        // Documents hold NVRHI resources -> declared after m_gpu (destruct
        // before the device) and after m_runtime (they borrow its Assets).
        std::unique_ptr<Arcane::ShaderCompiler> m_shaderCompiler;
        Arcane::ShaderSourceProvider            m_shaderSources;
        // Scene sprite materials (Slice 8): resolves SAVED .arcmat assets
        // referenced by SpriteRenderer::material into registered Batcher2D
        // materials; the drain site feeds it, the frame loop publishes its
        // table through Runtime::SetSpriteMaterials.
        std::unique_ptr<Arcane::SpriteMaterialCache> m_spriteMaterials;
        // Sprite-asset arc, Task 3: resolves SpriteRenderer::sprite's
        // .arcsprite Guids into Arcane::SpriteEntry records (texture + UVs +
        // size + pivot). Synchronous (no compile pipeline, unlike the
        // material cache above) -- constructed beside it from the same
        // services, swept in the same PumpShaderEditor loop, and published
        // through Runtime::SetSpriteTable.
        std::unique_ptr<Arcane::Editor::SpriteCache> m_sprites;
        // Scene post chain (post arc, slice 2): resolves the assigned post
        // material into a bound FullscreenMaterialChain for the viewport's
        // SetPostChain hook. Same drain site, same invalidation rides
        // (onAssetSaved + the material watcher); slice 3's PostProcess sweep
        // drives Request and hands Chain()/Instance() to the viewport.
        std::unique_ptr<Arcane::PostChainCache> m_postChains;
        Arcane::Editor::DocumentHost            m_documents;
        Arcane::Editor::AssetBrowserState       m_assetBrowser;
        double m_editorClock = 0.0;   // the compile service's Poll/Submit clock

        // Material file watcher: a ~1 Hz mtime sweep over the registry's
        // .arcmat files (ShaderLibrary's hot-reload pattern). An EXTERNAL
        // change (git pull, sibling repo, hand edit) invalidates the sprite
        // cache and reloads/refreshes open documents; our own saves
        // re-baseline through onAssetSaved so they never bounce back.
        void PollMaterialWatch();
        std::unordered_map<std::string, std::filesystem::file_time_type> m_materialMtimes;
        double m_materialWatchNext = 0.0;
        // >1 PostProcess assignments in the scene: warned once, reset when the
        // count drops back (the post sweep in UpdateScenePostChain).
        bool m_warnedMultiPost = false;

        // Async file-dialog results for the material flows (same background-
        // thread stash pattern as m_pendingProjectPath below).
        std::string m_pendingMaterialNewPath;
        std::string m_pendingMaterialOpenPath;
        std::string m_pendingInstanceNewPath;
        std::mutex  m_pendingMaterialMutex;
        // Parent for a pending "New Instance..." save dialog. Main-thread only:
        // written on the menu click, read when the dialog result lands.
        Arcane::Guid m_pendingInstanceParent;

        static void MaterialNewPickedThunk(const char* path, void* user);
        static void MaterialOpenPickedThunk(const char* path, void* user);
        static void InstanceNewPickedThunk(const char* path, void* user);
        // Mint a GRAPH-owned .arcmat (UE-model: nodes are the authoring tier)
        // + open its doc. Legacy text-owned files still open via OpenPath.
        void CreateMaterialAt(std::filesystem::path path);
        void CreateInstanceAt(std::filesystem::path path, Arcane::Guid parent);
        // Reuse-or-mint policy (sprite-asset spec, Section 3): exactly one
        // registered .arcsprite referencing `textureGuid` -> reuse its id;
        // zero or several -> mint a fresh sibling .arcsprite next to the
        // texture (never guess among duplicates). Nil on failure (no project,
        // invalid input, or an unresolvable/unwritable path). Never opens a
        // dialog; the caller (browser action consumer / Inspector drop
        // branch) decides whether to also open a document.
        Arcane::Guid MintOrReuseSpriteForTexture(const Arcane::Guid& textureGuid);
        Arcane::Editor::DocServices MakeDocServices();

        // The Arcane logo, shown at the left of the transport toolbar (Unity-style). A
        // display-referred (UNORM) texture -- NOT Assets::GetTexture's sRGB -- so it
        // composites correctly through the ImGui backend. Loaded once in Init; passed to
        // DrawSimTimeToolbar as an ImTextureID (the raw nvrhi::ITexture*). Null if the
        // image is missing (the toolbar simply omits it). Holds an NVRHI handle, so like
        // the other render resources it is declared after m_gpu (destructs before the device).
        nvrhi::TextureHandle m_toolbarLogo;

        // Deferred resize: the Viewport panel's content-region size measured LAST
        // frame, applied at the START of THIS frame (before m_viewport->Draw). This
        // mirrors the m_viewportRect/m_viewportActive one-frame lag above -- it avoids
        // a same-frame use-after-free where OffscreenCanvas::Resize (called right after
        // ImGui::Image bakes the current texture pointer into this frame's draw list)
        // synchronously frees that very texture before ImGui replays the draw list at
        // Render time. See ApplyPendingViewportResize for the full sequencing rationale.
        std::uint32_t m_pendingViewportW = 0;
        std::uint32_t m_pendingViewportH = 0;

        // File -> Open Project (soft-restart). The menu sets m_openProjectRequested;
        // DrawEditorUi launches the async .arcproj FILE dialog; SDL runs
        // ProjectPickedThunk on an SDL-owned BACKGROUND thread (NOT the main
        // thread -- the Windows backend's dialog callback fires off a detached
        // worker), which stashes the chosen path in m_pendingProjectPath under
        // m_pendingProjectMutex; the next frame's ConsumeProjectDialogResult takes
        // the lock, swaps the path out, and (outside the lock) runs SwitchProject
        // at a safe point. (Project::Open accepts the .arcproj file directly.)
        std::string m_pendingProjectPath;
        std::mutex  m_pendingProjectMutex;   // guards m_pendingProjectPath across the SDL callback thread

        static void ProjectPickedThunk(const char* path, void* user);  // -> m_pendingProjectPath (background thread)
        void        SwitchProject(const std::filesystem::path& path);  // validate-then-soft-restart

        // Async scene file-dialog results (same background-thread stash pattern as
        // m_pendingProjectPath: the SDL dialog backend fires its callback from a
        // detached worker, so these are swapped out at the top of the next frame
        // under the mutex).
        std::string m_pendingSceneOpenPath;
        std::string m_pendingSceneSavePath;
        std::mutex  m_pendingSceneMutex;

        static void SceneOpenPickedThunk(const char* path, void* user);
        static void SceneSavePickedThunk(const char* path, void* user);

        // Editor state naming entities of the OUTGOING scene, torn down before any
        // registry swap. Shared by SwitchProject and the scene effects below.
        void ClearSceneReferences();
        // Establish an empty scene when nothing published a SceneRoot, so the editor
        // always has one open. Never clears a registry a plugin already populated.
        void EnsureScene();

        // Scene dialog launches, shared by the menu-request site and the
        // unsaved-scene confirm modal (they were one lambda pair inside
        // MainLoop, called from both).
        std::string SceneDialogDir();
        void        ShowSceneSaveDialog();

        // The scene effects. Each returns false when the effect itself failed, and
        // sets m_sceneError to the reason (DrawModals draws it as a modal).
        bool DoNewScene();
        bool DoOpenScene(const std::filesystem::path& file);
        bool DoSaveScene(const std::filesystem::path& file);

        // Write <projectRoot>/Saved/AutoScreenshot.png from the viewport
        // output -- the editor half of the Arcane Hub's cover-thumbnail
        // chain (Unreal's model). Called on scene save and on clean
        // shutdown; a no-op without a project, a viewport, or a device.
        void WriteAutoScreenshot();

        // Last scene failure, shown as a blocking modal until dismissed. Main
        // thread only (set in the effects, read in the ImGui pass).
        std::string m_sceneError;

        // Window title, recomposed from project + scene + dirty state each frame
        // and pushed to the window only when it changes (SetTitle is an SDL call,
        // and the string is identical on the overwhelming majority of frames).
        std::string m_windowTitle;
        void        UpdateWindowTitle();

        // The dock node the Viewport occupied LAST frame (0 = floating):
        // where new document windows dock as sibling tabs (DrawAll).
        unsigned int m_viewportDockId = 0;

        // ---- Material panel: which document it shows -----------------------
        // The material document whose center tab is active, or the last one
        // that was. Held as a GUID rather than a pointer because DocumentHost
        // destroys documents synchronously on close (DocumentHost::Close erases
        // the unique_ptr), so a cached raw pointer would dangle for the rest of
        // the frame; the guid is re-resolved through FindByGuid every frame.
        // Nil = no material open, so the panel is not submitted at all.
        Arcane::Guid m_activeMaterialGuid;
        // Open material-document count LAST frame, so "the last one closed" is
        // a detectable edge even when the Viewport does not report Appearing
        // (a document that was floating rather than tabbed over the scene).
        std::size_t  m_materialDocCount = 0;

        // Open-failure surfacing: SwitchProject's refusals used to be console-only
        // and were repeatedly missed at the desk. Any refusal/failure sets this;
        // DrawModals shows it as a blocking modal (main thread only -- set inside
        // SwitchProject/Init, read in the ImGui frame; no lock needed).
        std::string m_projectOpenError;

        // One-shot latch for RaiseOpenProjectOnStart: consumed on the first frame
        // that draws the menu bar, so the picker appears over a live editor window
        // rather than before one exists.
        bool m_raiseOpenProjectOnStart = false;
    };
}
