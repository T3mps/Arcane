#pragma once
// EditorApp: the editor application. Constructed in main from a LoomConfig
// (reused as the host config); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors Loom.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <LoomConfig.hpp>
#include <GpuContext.hpp>
#include <FramePerf.hpp>
#include "AssetBrowser.hpp"
#include "ConsoleBuffer.hpp"
#include "DocumentHost.hpp"
#include "EditorPanels.hpp"
#include "PlayMode.hpp"
#include "SceneSession.hpp"
#include "SelectionContext.hpp"
#include "ShaderEditorDocument.hpp"
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

namespace Arcane::Editor
{
    class EditorApp
    {
    public:
        explicit EditorApp(LoomConfig cfg);
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

        LoomConfig                        m_config;
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
        // valid. See MainLoop.
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
        // across frames -- see MainLoop's input block).
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
        // Set for the remainder of THIS frame when a gizmo drag starts or ends,
        // so the click-pick block (later in MainLoop) does not also treat the
        // same click as a selection change. Reset at the top of the input block
        // each frame.
        bool m_gizmoCapturedClick = false;

        // Scene-in-a-panel viewport: the same canvas->batcher->tonemap path Loom
        // drives the backbuffer with, rendered into a panel texture instead. Resized
        // to the Viewport panel's content region each frame; input into the plugin
        // is gated on m_viewportActive and remapped through m_viewportRect (see
        // ViewportInput.hpp + MainLoop's input block).
        std::unique_ptr<Arcane::OffscreenCanvas> m_viewport;
        Arcane::Editor::ViewportRect                   m_viewportRect{};
        bool                                     m_viewportActive = false;

        // Viewport-local input snapshot for the game ImGui pass, captured inside
        // MainLoop's input block (whose locals are out of scope at the render
        // site) and read where the game UI is composited into the viewport. Only
        // the few values the game context needs are hoisted -- the input block's
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
        // (amber selected, cyan hovered). See MainLoop, right after
        // m_viewport->Draw -- the same slot the Play-only game-imgui overlay pass
        // uses (the two are mutually exclusive by mode).
        std::unique_ptr<Arcane::SelectionOutline> m_outline;

        // Shader-editor services + open documents (Slice 5). The compiler is the
        // app-shared compile service (documents Submit through it; MainLoop
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
        // count drops back (the post sweep in MainLoop).
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
        // Render time. See MainLoop for the full sequencing rationale.
        std::uint32_t m_pendingViewportW = 0;
        std::uint32_t m_pendingViewportH = 0;

        // File -> Open Project (soft-restart). The menu sets m_openProjectRequested;
        // MainLoop launches the async .arcproj FILE dialog; SDL runs
        // ProjectPickedThunk on an SDL-owned BACKGROUND thread (NOT the main
        // thread -- the Windows backend's dialog callback fires off a detached
        // worker), which stashes the chosen path in m_pendingProjectPath under
        // m_pendingProjectMutex; the next frame's top of MainLoop takes the
        // lock, swaps the path out, and (outside the lock) runs SwitchProject
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

        // The scene effects. Each returns false when the effect itself failed, and
        // sets m_sceneError to the reason (MainLoop draws it as a modal).
        bool DoNewScene();
        bool DoOpenScene(const std::filesystem::path& file);
        bool DoSaveScene(const std::filesystem::path& file);

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

        // Open-failure surfacing: SwitchProject's refusals used to be console-only
        // and were repeatedly missed at the desk. Any refusal/failure sets this;
        // MainLoop shows it as a blocking modal (main thread only -- set inside
        // SwitchProject/Init, read in the ImGui frame; no lock needed).
        std::string m_projectOpenError;

        // One-shot latch for RaiseOpenProjectOnStart: consumed on the first frame
        // that draws the menu bar, so the picker appears over a live editor window
        // rather than before one exists.
        bool m_raiseOpenProjectOnStart = false;
    };
}
