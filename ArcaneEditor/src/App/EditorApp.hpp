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
#include <Arcane/Host/BootPresenter.hpp>
#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include "Panels/AssetBrowser.hpp"
#include "Panels/ConsoleBuffer.hpp"
#include "Panels/DiagnosticStore.hpp"
#include "App/DialogSlot.hpp"
#include "App/InputEdges.hpp"
#include "App/ModalErrorQueue.hpp"
#include "Documents/DocumentHost.hpp"
#include "Viewport/EditorCamera.hpp"
#include "Panels/EditorPanels.hpp"
#include "Project/ModuleBuild.hpp"
#include "App/PlayMode.hpp"
#include "Panels/ProblemsPanel.hpp"
#include "Project/EditorRecents.hpp"
#include "Scene/SceneSession.hpp"
#include "Scene/SelectionContext.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Viewport/DeferredPick.hpp"
#include "Viewport/ViewportInput.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/ImGui/OffscreenImGuiLayer.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>   // dev-only Build -> Diagnostics -> Crash GPU
#include <Arcane/Render/Nri/NriGraphContext.hpp>   // dev-only --nri-graph vehicle (chrome + viewport)
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/PickEmit.hpp>   // PickDrawable -- the graph arm's per-frame pickables
#include <Arcane/Render/PickBuffer.hpp>
#include <Arcane/Render/SelectionOutline.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <spdlog/sinks/callback_sink.h>

namespace Astra { class TypeContext; }
namespace Arcane { struct InputSnapshot; }   // by-reference phase parameters only
// Opaque here on purpose: the play-mode settings handler (Task 6) is the only
// thing in this header that needs these names, and ImGui's ini extension
// point (ImGuiSettingsHandler) is internal-only -- imgui_internal.h stays
// confined to EditorApp.cpp, never leaking into this header. ImGuiContext is
// forward-declared in the public imgui.h too; ImGuiTextBuffer is a complete
// type there, but only a pointer to it appears below, so this is enough.
struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace Arcane::Editor
{
    class EditorApp
    {
    public:
        // `splash` is a NON-OWNING pointer to main()'s stack-local
        // Arcane::BootSplashWindow (Task 8, async-boot arc): main constructs
        // and ultimately closes it (BootSplashWindow's ordering contract
        // requires both from "main" -- never under the loader lock, never from
        // two threads at once); EditorApp only reads it during Run()'s boot
        // sequence and calls Close() once, synchronously, from the
        // splash_ready stage (still on main's call stack -- see Run()). Null
        // is tolerated (no splash to hand off).
        explicit EditorApp(HostConfig cfg, Arcane::BootSplashWindow* splash = nullptr);
        int Run();   // BootSequence -> MainLoop() -> Shutdown(); process exit code

        // Raise File -> Open Project on the FIRST frame. Set by main() for a bare
        // interactive launch (no --project, no --plugin): the editor supports the
        // project-less state, and this is its cold-start path into the picker.
        void RaiseOpenProjectOnStart() noexcept { m_raiseOpenProjectOnStart = true; }

    private:
        // ---- Boot (EditorApp.cpp) -------------------------------------------
        // Run() builds Arcane::HostBoot::EditorStages(ctx) -- the SAME shared
        // function RuntimeApp calls for its own list -- then overwrites the
        // ids below with these closures, because their real work touches
        // EditorApp's own private members (m_gpu/m_runtime/m_plugin/...) or
        // editor-exe-only types (EditorTheme/EditorFonts/ShaderEditorDocument)
        // that Arcane.dll cannot see. type_context_install/project_open/
        // input_config/editor_lock are NOT in this list -- their
        // CoreStages/EditorStages body is genuinely shared and used as-is; see
        // ProjectBoot.cpp for why each stage landed where it did. Every method
        // returns false only where the equivalent Init() block used to.
        //
        // splash_ready IS in this list now (Task 8c, 2026-07-30 correction:
        // "the splash carries the loading UI, not the editor window") --
        // revealing the window needs m_presenter, a host-owned member
        // ProjectBoot.cpp's ctx-only lambda cannot reach, exactly the same
        // structural reason render_bridge/plugin_load/etc. are host-owned.
        // EditorStages() installs Make()'s Unpatched(id) sentinel for
        // splash_ready's `run` (empty by default) for the same reason it does
        // for every other host-owned id -- a host that forgets to patch it
        // fails loudly instead of quietly skipping the reveal.
        bool StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx);
        bool StageGpuCore(Arcane::HostBoot::BootContext& ctx);
        bool StageEditorFonts(Arcane::HostBoot::BootContext& ctx);
        bool StageEditorShell(Arcane::HostBoot::BootContext& ctx);
        bool StageRenderBridge(Arcane::HostBoot::BootContext& ctx);
        bool StageEditCore(Arcane::HostBoot::BootContext& ctx);
        bool StageSpriteTables(Arcane::HostBoot::BootContext& ctx);
        bool StagePluginLoad(Arcane::HostBoot::BootContext& ctx);
        bool StageFinalize(Arcane::HostBoot::BootContext& ctx);
        bool StageSplashReady(Arcane::HostBoot::BootContext& ctx);

        enum class InitResult
        {
            Ok,       // boot completed; run the frame loop
            Failed,   // a Fatal stage failed, or the patch table drifted -> exit 1
            Quit,     // the user closed the splash mid-boot -> exit 0, not an error
        };

        // THE host-owned stage patch (architecture pass sec 5): Create() applies it
        // to the boot list; SwitchProject (Task 12) applies it to the same shared
        // EditorStages(ctx) list before cherry-picking its subset -- ONE patch
        // path, both directions of drift still fail loudly (see the table's own
        // comment in the .cpp). False = a patch id matched no stage -- table
        // drift; the caller must fail loud (Create() returns false; a future
        // SwitchProject call is expected to do the same).
        bool PatchHostStages(std::vector<Arcane::BootStage>& stages);

        bool       Create();
        InitResult Init();
        // THE GRAPH VEHICLES' CREATE, run by Main() between boot and the frame
        // loop (--nri-graph only; a no-op returning true on the NVRHI arm).
        // It sits HERE rather than in a boot stage for the ordering
        // RuntimeApp::MainLoop proved at three desk checkpoints: the window is
        // Show()n FIRST and the swapchain built over an already-mapped window,
        // because a surface created against a window the compositor has never
        // seen is a backend-specific corner a desk-only machine cannot
        // pre-clear. The editor's own reveal (StageSplashReady) is
        // NVRHI-backed and skips itself on this flavor for the same reason
        // RuntimeApp::StageFinalize does. False = already logged; Main() turns
        // it into exit 1.
        bool       CreateGraphVehicles();
        // The VIEWPORT context and every seam it needs, with TWO callers (NRI
        // Phase 3, Task 12): CreateGraphVehicles at boot, and SwitchProject's
        // "render_bridge" stage on every project switch -- which rebuilds this
        // context from scratch rather than flushing it (TeardownGraphForSwitch
        // states the kept-vs-torn decision in full). Shared rather than copied
        // because three of its four statements fail SILENTLY when missed: a
        // missing AdoptImGuiContext is a blank plugin HUD, a missing resolver
        // or pixel supply is a white texel on every sprite. False = already
        // logged; both callers turn it into a loud stop.
        bool       BuildGraphViewportContext(std::uint32_t width, std::uint32_t height);
        int        Main();
        void       Shutdown();
        void       Destroy();

        void MainLoop();

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
            // drift apart. Same shape for the Edit-menu clipboard shortcuts
            // (Ctrl+X/C/V/D) below.
            bool scNewScene = false, scOpenScene = false, scSaveScene = false;
            bool scCut = false, scCopy = false, scPaste = false, scDuplicate = false;
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
        void RefreshSceneResolution();
        void RenderSceneToViewport();
        // The scene's 2D submission plus the two Edit-mode overlays drawn on
        // top of it (the scene-camera rect and the transform gizmo), against
        // whichever batcher the caller hands in. Extracted VERBATIM from
        // RenderSceneToViewport's OffscreenCanvas::Draw lambda so the two
        // recorders drain identical content: the NVRHI arm passes the canvas'
        // batcher from inside Draw, the graph arm passes the device-less
        // GpuContext batcher it Begin()s itself. Contains no target, no
        // command list and no device -- that is what makes it shareable.
        void SubmitSceneToBatcher(Arcane::Batcher2D& b);
        // Phase 11's CPU HALF, shared by both arms (NRI Phase 3, Task 9): the
        // game context's per-frame input, and -- in Play, with a plugin that
        // draws one -- its BeginFrame + DrawUIAll. Returns true when a game-UI
        // frame was BEGUN and therefore owes exactly one Render /
        // RenderToDrawData / EndFrameDiscard; false when nothing was begun.
        //
        // Extracted so the two arms run the SAME statements in the same order
        // against the same context, differing only in who renders the result:
        // the NVRHI arm blits through OffscreenImGuiLayer::Render (phase 11),
        // the graph arm hands RenderToDrawData()'s output to a graph node
        // (phase 10). Same discipline, and the same reason, as
        // SubmitSceneToBatcher.
        [[nodiscard]] bool BeginGameUiFrame();
        // The graph arm's FrameDesc arming for the pick + outline chain and the
        // game HUD -- i.e. everything phases 11, 12 and 17 contribute to a frame
        // that is DECLARED in phase 10. Fills the members the FrameDesc's spans
        // point at (m_pickDrawables / m_pickSelectedIds) and returns nothing by
        // value, because a span into a temporary is the one mistake this shape
        // exists to make impossible.
        void ArmGraphViewportFrame(Arcane::NriGraphContext::FrameDesc& vp);
        void CompositeGameUi();
        void RenderSelectionOutline();
        void PumpEditorDocuments();
        void DrawEditorUi(LoopState& ls, const FrameState& fs);
        void ConsumeMenuRequests(Arcane::Editor::MenuRequests& menuReq,
                                 const FrameState& fs, LoopState& ls);
        void ConsumeBrowserActions(const Arcane::Editor::AssetBrowserActions& browserActions,
                                   LoopState& ls);
        Arcane::Editor::ShaderEditorDocument* ResolveActiveMaterialDoc();
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
        // Phase 19's GRAPH arm (NRI Phase 3, Task 10): the editor's main-window
        // frame as a graph frame -- clear + one ImGui node over the EDITOR
        // context's draw data -> the chrome context's backbuffer -> present.
        // Same return contract as PresentFrame (false = nothing was presented,
        // MainLoop skips the rest of the frame). Its own definition carries the
        // frame's declared shape and the four fields deliberately left default.
        bool PresentChromeFrame();
        // NRI Phase 3, Task 13: the NVRHI arm's half of the editor golden
        // harness. Targets the last frame of a golden run, right after the
        // canvas output texture has its final content for the frame
        // (CompositeGameUi/RenderSelectionOutline are the last writers into
        // it) -- reads it back via Arcane::ReadTexturePixels and hands the
        // pixels to Arcane::GoldenArtifact (Host/GoldenHarness.hpp), exactly
        // the pairing RuntimeFrame::CaptureTail uses for the NVRHI backbuffer.
        // A no-op on the graph arm, where the equivalent capture is armed as
        // a NODE inside RenderSceneToViewport's own FrameDesc instead (see
        // that method) -- there is no separate "read this texture" entry
        // point on that recorder, so piggybacking on the frame already being
        // declared is the only shape that exists.
        //
        // NOT ALWAYS EXACTLY ONCE, and the correction is deliberate (review
        // fix round 1): this phase sits BEFORE PresentFrame in MainLoop's
        // order, and PresentFrame can fail to acquire a backbuffer (a
        // transient zero-size/out-of-date swapchain) and return false, which
        // makes MainLoop `continue` WITHOUT running EndFrame -- so
        // m_frameCount does not advance and the "is this the last frame"
        // test above fires again, unchanged, on the retry iteration. The
        // retry re-reads the (unchanged) canvas and re-runs GoldenArtifact,
        // which simply overwrites the same file with equivalent content --
        // harmless, not a correctness bug, and orthogonal to the backbuffer
        // failure that caused the retry (this phase never touches the
        // swapchain at all). m_goldenCaptured (below) is set on every
        // attempt, retries included, which is exactly right: each retry IS a
        // genuine capture attempt against a real target.
        void CaptureEditorGolden();
        void EndFrame(LoopState& ls);

        // ---- The graph arm's EXIT-CODE FOLD (NRI Phase 3, Task 10) ----------
        // The editor's counterpart of RuntimeApp::m_graphExit + its
        // ShutdownGraphPath, and deliberately the same codes and the same
        // precedence (1 > 2): 1 = a graph frame FAILED (it says WHERE the run
        // died), 2 = RenderErrorCount GREW across the run, teardown included.
        //
        // Both CONTEXTS fold into the one code -- a viewport frame that could
        // not be recorded (RenderSceneToViewport, phase 10) and a chrome frame
        // that could not be presented (PresentChromeFrame, phase 19) are the
        // same class of failure and neither was visible in an exit code before.
        void NoteGraphFrameFailure(const char* what);
        // ---- The project switch's graph teardown (NRI Phase 3, Task 12) -----
        // The graph-mode stand-in for the NVRHI arm's single `waitForIdle` in
        // SwitchProject's "switch_teardown" stage: an ordered idle ->
        // invalidate -> release (whose own drain closes the sequence), with
        // the chrome context and m_gameImgui deliberately KEPT and the
        // viewport context deliberately DESTROYED. The full kept-vs-torn
        // decision, the reason a content flush is not an option, and the
        // adjacency rule that makes the ordering safe are all stated at the
        // definition -- read it before changing either end of the switch.
        // Hands back the outgoing viewport extent so the rebuild in
        // "render_bridge" can recreate at the size the panel is actually
        // showing; 0/0 means "use the boot default". A no-op on the NVRHI arm
        // (no chrome context) -- the caller gates on GraphMode() anyway.
        void TeardownGraphForSwitch(std::uint32_t& keepWidth, std::uint32_t& keepHeight);
        // Destroys BOTH graph contexts, in the one order that is correct, and
        // then reads the latch back -- which is the whole reason it exists as a
        // function rather than as member destruction: a teardown-only
        // validation error must still fail the run, and member destructors run
        // after Run() has already returned its code. Mirrors
        // RuntimeApp::ShutdownGraphPath. A no-op on the NVRHI arm.
        void ShutdownGraphPath();

        HostConfig                        m_config;
        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST

        // ---- The graph vehicles (--nri-graph, NRI Phase 3, Task 8) ----------
        // THE CHROME CONTEXT: the host-window NriGraphContext. It owns the
        // process's ONLY graphics device (GpuContext::CreateForGraph builds no
        // NVRHI device at all), arms the crash chain, and its swapchain binds
        // m_gpu's window -- exactly what RuntimeApp::m_graphContext is. Null on
        // every ordinary run: the editor's NVRHI path is the default and the
        // floor.
        //
        // MID-PHASE, IT RENDERS NOTHING. Task 10 owns the editor's chrome
        // frame (clear + one ImGui node over the editor context's draw data ->
        // backbuffer -> present). Until then this object exists for two
        // reasons that are already load-bearing:
        //   * it is what CREATES the shared NriDevice the viewport context
        //     below borrows, and
        //   * ImGuiHud() is the backend that will cache the viewport's output
        //     texture, so it is the node the resize contract's
        //     InvalidateUserTextureNow must be called on -- see
        //     ViewportTargets::ApplyPendingResize.
        //
        // NOT #if-guarded even though the flag is non-Dist, for exactly the
        // reason RuntimeApp.hpp states for its own member: the type is compiled
        // into the engine DLL in every configuration, and a preprocessor-guarded
        // MEMBER would force every use site in the frame body to grow a guard of
        // its own -- which is how a Dist-only compile break gets introduced.
        // Only the CREATION is guarded, so a Dist build carries a null pointer
        // and one predictable branch.
        //
        // Declared AFTER m_gpu so it destructs BEFORE it, and that ordering is
        // LOAD-BEARING: this object's swapchain is bound to the window inside
        // m_gpu (NriGraphContext.hpp, THE BORROWED WINDOW). It is also declared
        // BEFORE m_viewportTargets, whose offscreen context BORROWS the device
        // this one owns -- so the borrower (declared later) destructs first.
        //
        // NOT on ResetPerProjectState's list, and neither is the viewport
        // context below -- the same ruling m_gpuFault and the NVRHI viewport
        // trio already carry: these hold DEVICE resources, not project ones,
        // and the device outlives a project switch.
        //
        // ===== AND THIS ONE SURVIVES A SWITCH OUTRIGHT (Task 12) =====
        // The decision, made and recorded rather than left implicit: THIS
        // context is window/device/session-scoped and a project switch does
        // not touch it, while the VIEWPORT context below is project-scoped in
        // its caches and is destroyed and rebuilt by every switch. Nothing
        // this object holds can name an asset of the outgoing project -- its
        // NriTextureCache's only possible tenant is the toolbar logo, whose
        // pixel supply answers exactly one synthetic per-run Guid and null for
        // everything else (CreateGraphVehicles), and it is handed no asset
        // resolver at all. What a switch DOES owe it is the ordered idle ->
        // invalidate -> release, with the invalidate made ON THIS OBJECT'S
        // ImGuiHud() for every texture the switch is about to destroy: it is
        // the backend that caches the viewport output and the document
        // previews by raw pointer. EditorApp::TeardownGraphForSwitch carries
        // the whole sequence and the argument for the split.
        std::unique_ptr<Arcane::NriGraphContext> m_graphChrome;

        // ---- Closed documents' preview vehicles (NRI Phase 3, Task 11) ----
        // A shader document on the graph arm owns an offscreen
        // NriGraphContext, and it is destroyed INSIDE the editor's ImGui pass
        // (DocumentHost::DrawAll erases the unique_ptr right after its draw
        // loop) -- while this frame's draw lists still name its output texture
        // by raw pointer and the chrome frame that replays them has not been
        // recorded. So the dying document hands the vehicle here instead, and
        // DrainRetiredDocPreviews destroys it at the TOP of the next frame,
        // invalidate first. Full reasoning: DocServices::retireGraphPreview.
        //
        // DECLARED HERE, AND THE POSITION IS THE POINT (fix round 1). It has to
        // sit BETWEEN m_graphChrome and m_documents, because reverse-order
        // destruction then runs:
        //     ~m_documents  ->  ~m_retiredDocPreviews  ->  ~m_graphChrome
        // i.e. a document destroyed by ~EditorApp can still push its vehicle in
        // here, and every vehicle in here still dies before the context that
        // owns the device it borrows. Declared after m_documents (where it
        // first was) the vector would be GONE by the time ~ShaderEditorDocument
        // pushed into it.
        //
        // WHAT THAT ORDER DOES NOT PROVIDE, stated rather than glossed: ~vector
        // destroys a vehicle WITHOUT the chrome-side InvalidateUserTextureNow,
        // which only DrainRetiredDocPreviews does. That is not a live hole --
        // a vehicle exists only if MakeDocServices saw m_graphChrome, nothing
        // nulls it before Shutdown, and ShutdownGraphPath therefore always runs
        // its CloseAll + drain -- so the destructor path is unreachable with a
        // non-empty vector. The declaration order is what keeps it SAFE rather
        // than merely unreached.
        //
        // THE PROJECT SWITCH IS THE SECOND CloseAll + drain PAIRING (Task 12):
        // ResetPerProjectState's CloseAll fills this list mid-switch, and
        // TeardownGraphForSwitch drains it in the same stage -- not at the next
        // frame's phase 13, which is across a plugin unload and a project open.
        std::vector<std::unique_ptr<Arcane::NriGraphContext>> m_retiredDocPreviews;
        void RetireDocPreview(std::unique_ptr<Arcane::NriGraphContext> vehicle);
        void DrainRetiredDocPreviews();

        // The graph arm's own exit code, 0 on every ordinary run -- see
        // NoteGraphFrameFailure / ShutdownGraphPath above and Run()'s tail,
        // which is where it reaches the process. Not #if-guarded, for the same
        // reason m_graphChrome is not: a preprocessor-guarded member forces a
        // guard at every use site.
        int           m_graphExit = 0;
        // RenderErrorCount at the moment the chrome context was created --
        // BOOT-time errors belong to the boot, and everything from there until
        // the last NRI object is gone belongs to the graph. Read back in
        // ShutdownGraphPath, which prints the pair as one line so the desk
        // watch item ("the count must not grow across a clean exit") is legible
        // rather than inferred.
        std::uint64_t m_graphErrorBaseline = 0;

        // ---- The editor's golden harness (NRI Phase 3, Task 13) -------------
        // The editor's counterpart of RuntimeApp::m_goldenExit: 0 ordinarily;
        // set to 3 by the golden warm-up's census refusal (MainLoop, before
        // the frame loop starts) or by a capture/compare failure on the last
        // frame (CaptureEditorGolden on the NVRHI arm, RenderSceneToViewport
        // on the graph arm) -- the SAME exit code RuntimeApp::Run reports for
        // the identical failure, and the same Arcane::GoldenArtifact call
        // produces it on both hosts. Read by Run()'s tail, OUTRANKED by
        // m_graphExit (precedence 1 > 2 > 3, matching RuntimeApp::Run's own
        // 1 > 2 > 3 -- a run failure says WHERE the run died and a validation
        // error explains a bad capture rather than the reverse, so both
        // outrank a golden mismatch). Not #if-guarded, for the same reason
        // m_graphExit is not: a preprocessor-guarded member forces a guard at
        // every use site.
        //
        // EXIT CODE 3 COLLIDES WITH main.cpp's PRE-BOOT "rival editor" exit
        // 3 from ArcaneHub's launch.rs's point of view (its boot watchdog
        // reads any exit inside 2s as the pre-boot meaning) -- see the full
        // exit-code table and reconciliation in ArcaneEditor/src/main.cpp,
        // right above `int main`. Kept identical to RuntimeApp's own golden
        // exit code deliberately (this task's contract); not a defect to fix
        // here.
        int m_goldenExit = 0;
        // NRI Phase 3, Task 13 fix round 1 (IMPORTANT, review finding 2):
        // latches "a capture was genuinely ATTEMPTED", set by BOTH capture
        // paths (CaptureEditorGolden, the NVRHI arm; RenderSceneToViewport's
        // capture block, the graph arm) the moment either one confirms this
        // is the run's last frame AND has a real target to read -- NOT
        // merely that the last-frame arithmetic fired. Without this, a
        // golden run whose last iteration's viewport frame came back
        // Skipped (graph arm, a collapsed/zero-sized panel) or whose canvas
        // was already gone (NVRHI arm, a lost viewport context) would reach
        // maxFrames with m_goldenExit still 0 and exit 0 -- a PASS that
        // compared NOTHING. Checked once, in MainLoop right after the frame
        // loop exits (a whole-run property, not a per-frame one): `if
        // (GoldenMode() && !m_goldenCaptured) m_goldenExit = 3;`.
        bool m_goldenCaptured = false;

        // Pre-device splash (Task 8): non-owning, see the ctor's doc comment.
        // Task 8c: this is now BootSequence::Run's presenter for the WHOLE
        // boot (via m_splashPresenter below, a MEMBER constructed from this
        // pointer -- not, as this comment claimed until Task 8d, a local
        // Run() builds; see that member's own note for why it cannot be a
        // local) -- not merely a pre-device stand-in. The old LazyBootPresenter nested class that used to live
        // here is gone: it existed to defer binding a swapchain-backed
        // presenter until StageEditorShell had installed fonts/theme, which
        // mattered only because BootSequence used to drive THAT presenter's
        // Present() automatically after every stage. It no longer does --
        // the swapchain-backed m_presenter below is now used exactly once,
        // explicitly, by StageSplashReady, so there is nothing left to defer.
        Arcane::BootSplashWindow*             m_splash = nullptr;
        // A class member, not a Run()-local (2026-07-30 review round 2,
        // finding 2): StageSplashReady needs to call Disarm() on THIS exact
        // instance right before it closes the splash itself, and Disarm()
        // has to reach it through `this`, not through BootSequence's opaque
        // IBootPresenter*. Declared right after m_splash (constructed from
        // it, in declaration order, in the ctor's init list below) -- has no
        // device dependency, so unlike m_presenter it can exist for the
        // App's whole lifetime rather than being emplaced late.
        Arcane::BootSplashPresenter           m_splashPresenter;
        // Cannot be constructed before StageGpuCore builds m_gpu. Emplaced
        // lazily inside StageSplashReady, the one place it is used -- see
        // that method's body for why the old "must not bind too early"
        // hazard this comment used to describe no longer applies.
        std::optional<Arcane::BootPresenter>  m_presenter;

        // Captured at the end of StageGpuCore (right after GpuContext::Create
        // has created the editor's ImGuiLayer, which is the only ImGui
        // context in existence at that point) and checked against
        // ImGui::GetCurrentContext() at the top of StageEditorShell
        // (2026-07-30 review, Fix 1's verification requirement): a
        // permanent regression tripwire for the exact bug this fix closes --
        // if some future boot-stage reordering lets plugin_load (or anything
        // else that can call ImGui::SetCurrentContext) run before
        // editor_shell again, this fires immediately with a clear cause
        // instead of degrading into wrong fonts/theme and a much later,
        // much harder to diagnose DockBuilderAddNode crash.
        ImGuiContext*                          m_editorImguiContext = nullptr;

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
        //
        // NON-NULL ON BOTH ARMS AGAIN AS OF NRI PHASE 3, TASK 9, and that is
        // the point of the whole game-UI half of that task rather than a detail
        // of it. Task 8 left this null on the graph arm (OffscreenImGuiLayer was
        // an ImGui-NVRHI object) and the plugin was handed the EDITOR's context
        // instead -- whose io.IniFilename is the PER-PROJECT LAYOUT FILE, where
        // this one's is deliberately null. A plugin window submitted through
        // that fallback would have been persisted into the user's layout,
        // permanently, and per-id ini state silently overrides authored UI on
        // every later boot. The graph arm now builds the SAME class through
        // OffscreenImGuiLayer::CreateForGraph -- same context, same io, same
        // pinning discipline, no NVRHI renderer -- and the graph's ImGuiNriNode
        // draws its RenderToDrawData() output. The isolation is structural
        // again on both arms; nothing is holding it by control flow.
        //
        // ===== AND ITS POSITION IN THIS LIST IS LOAD-BEARING ON THE GRAPH ARM
        // (Task 9 fix round 1). The viewport context's game ImGuiNriNode ADOPTS
        // this context, and ImGuiNri::Release PINS the adopted context to walk
        // its platform texture list -- which is a DEREFERENCE. So THIS OBJECT
        // MUST OUTLIVE THAT NODE'S Release. It does, and by construction rather
        // than luck: it is declared here, far ABOVE m_viewportTargets (which
        // owns the offscreen NriGraphContext), so reverse-order destruction
        // runs ~NriGraphContext -- and the Release inside it -- while this is
        // still alive. Do not move either declaration past the other.
        //
        // TASK 12 DISCHARGED THE SAME ORDERING EXPLICITLY, in both halves:
        // switch_teardown (EditorApp::TeardownGraphForSwitch) destroys the
        // VIEWPORT context -- and therefore that node's Release, which PINS
        // this context -- WITHOUT touching this member, which is why nothing
        // in that function resets it; and the rebuild re-issues
        // ImGuiNriNode::AdoptImGuiContext on the new node
        // (BuildGraphViewportContext), without which the HUD goes silently
        // blank after the first switch. The full obligation is stated on
        // ImGuiNri's m_imguiContext member.
        std::unique_ptr<Arcane::OffscreenImGuiLayer> m_gameImgui;

        Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
        std::optional<Arcane::Runtime>    m_runtime;                // destructs before m_gpu
        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;

        // ---- Boot state (EditorApp::Create/Init) ----------------------------
        // Deliberately declared HERE, outside the m_gpu/m_runtime/m_plugin
        // block above: those three participate in the load-bearing reverse-
        // declaration teardown contract documented in Shutdown(), and these two
        // take no part in it (Destroy() releases m_bootSeq explicitly, before
        // any member destructor runs).
        //
        // Both must outlive Create(): every patched stage's `run` captures
        // `this` and reads m_bootCtx, and those closures live inside m_bootSeq,
        // which Init() drives after Create() has returned.
        //
        // CONSTRAINT: m_bootCtx holds c_str() views into m_config
        // (projectPath/pluginPath) for this object's whole lifetime, so
        // m_config must not be mutated after construction. It is not today --
        // the only writes are in the ctor's initializer list -- and that is now
        // load-bearing rather than incidental.
        Arcane::HostBoot::BootContext       m_bootCtx{};
        // emplace-only: BootSequence deletes its copy constructor, which
        // suppresses the implicit move too, so it is neither copyable nor
        // movable -- optional can still hold it, but only via emplace().
        std::optional<Arcane::BootSequence> m_bootSeq;
        // Set by Init() on a successful BootSequence::Run. Read by Shutdown()
        // to decide whether this process owns the editor lock it is about to
        // clear -- see that call site.
        bool                                m_bootCompleted = false;

        // Console buffer + engine sink, and the Problems-panel diagnostic
        // STATE (distinct from the console's append-only log stream), grouped
        // together because Install()/Uninstall() are called from the same two
        // sites (Create()/Shutdown()) for the same "nothing may dispatch into
        // a half-torn-down editor" reason. See those methods (EditorApp.cpp)
        // for the create/shutdown timing rationale.
        struct ConsoleDiagnostics
        {
            ConsoleBuffer                                    console{512};
            std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;      // erased in Uninstall
            Arcane::Editor::ConsoleUiState                   ui;
            Arcane::Editor::DiagnosticStore                  store;
            Arcane::Editor::ProblemsUiState                  problemsUi;
            void Install();     // console sink + store.InstallAsEngineSink (was
                                // InstallConsoleSink + the Create() call beside it)
            void Uninstall();   // the Shutdown-top erase/uninstall pair
        };
        ConsoleDiagnostics m_consoleDiag;

        // Play-in-editor (Task 8): Edit|Play state machine. Play() snapshots the
        // registry + unpauses the RunLoop; Stop() restores the snapshot + re-pauses.
        // Arcane Editor boots in Edit (see Init: the RunLoop is paused right after the
        // plugin loads).
        Arcane::Editor::PlaySession m_play;

        // THE Play/edit predicate (architecture pass sec 1). Editor code asks this,
        // never m_play.IsPlaying() raw, so the predicate has one greppable name.
        [[nodiscard]] bool InPlayMode() const noexcept { return m_play.IsPlaying(); }

        // ---- THE graph/NVRHI predicate (--nri-graph, NRI Phase 3, Task 8) ---
        // The editor's counterpart of RuntimeApp's `m_gpu->GraphFlavor()`
        // branches, and the ONE name every mode gate in this host reads --
        // never HostConfig::nriGraph, for GpuContext.hpp's stated reason: what
        // matters at a call site is whether the NVRHI members EXIST, not which
        // flag caused that. Always false in a Dist build (CreateForGraph is
        // never reached there), so every gate below folds to the NVRHI arm.
        [[nodiscard]] bool GraphMode() const noexcept
        { return m_gpu && m_gpu->GraphFlavor(); }

        // ---- THE HOVER predicate (whole-branch review, C2) -------------------
        // "The pointer is over the Viewport panel AND that fact is allowed to
        // affect what is rendered." The second half is what this exists for:
        // m_gameUi.inViewport is re-derived from the LIVE cursor every frame
        // (FrameInput, EditorAppFrame.cpp), so a hover-driven outline makes the
        // frame a function of where the mouse happens to be sitting -- and a
        // GOLDEN artifact that depends on the mouse is not a baseline. The same
        // scripted command line otherwise produces `full` with a cyan hover
        // outline over whatever entity the pointer happens to rest on -- or
        // none at all when it rests elsewhere.
        //
        // STILL THE RIGHT PIN after the Task 13 follow-up gave `full` a
        // scripted SELECTION (PinGoldenViewport): what the outline traces is
        // now decided by that selection, deterministically, and hover would
        // only add a second, mouse-shaped source of pixels on top of it. The
        // two are different inputs to the same chain -- one is pinned OFF, the
        // other is pinned ON to a stable entity.
        //
        // Golden mode pins it OFF rather than pinning a coordinate: there is no
        // honest coordinate to pin (the panel's position is imgui.ini layout
        // state, saved per project GUID), and "no hover" is already the chain's
        // documented sentinel -- SelectionOutline::Params::cursorPx and
        // FrameDesc::hoverPixel both spell it (-1, -1).
        //
        // READ BY BOTH RENDER ARMS, and it has to be: ArmGraphViewportFrame
        // (graph) and RenderSelectionOutline (NVRHI -- the arm that captures
        // the D3c baselines) each carry phase 12's gate and each derive a
        // cursor from it, so a pin on one arm only would leave the two arms'
        // notion of `full` different, which is the one property the editor
        // golden harness exists to compare. Ordinary (non-golden) runs are
        // untouched: hover behaves exactly as before.
        [[nodiscard]] bool HoverLive() const noexcept
        { return m_gameUi.inViewport && !m_config.GoldenMode(); }

        // ---- THE GIZMO predicate (NRI Phase 3, Task 13 follow-up) -----------
        // "The transform gizmo may interact and draw." HoverLive()'s sibling,
        // and it exists for the same reason at one remove: the golden run now
        // SCRIPTS a selection (PinGoldenViewport), and a selection is exactly
        // what the gizmo waits for.
        //
        // TWO THINGS WOULD OTHERWISE FOLLOW FROM THAT SELECTION, both bad:
        //   * the gizmo is drawn by SubmitSceneToBatcher -- i.e. into the
        //     SCENE BATCH -- so it would appear in `batch` and `post` as well
        //     as `full`, and the scripted selection is only allowed to change
        //     `full`. There is no stage gate that could confine it: the
        //     batcher content is the batch stage.
        //   * m_gizmoHovered comes from a hit-test against the LIVE cursor
        //     (UpdateGizmoInteraction), so which axis is highlighted -- and
        //     whether a stray press starts a drag that MOVES the entity
        //     mid-run -- would depend on where the mouse was left. That is the
        //     hover-dependency class HoverLive() closed, arriving by a
        //     different door.
        //
        // NOT left to "m_gizmoEnabled defaults to false (the Select tool), so
        // a scripted run has no gizmo anyway". That is true, incidental, and
        // one W keypress away from false -- and "already false on a scripted
        // run" is the exact reasoning the whole-branch review's C2 caught being
        // wrong about the outline chain. Pinned, so it is a property rather
        // than a coincidence. Ordinary runs are untouched.
        [[nodiscard]] bool GizmoLive() const noexcept
        { return m_gizmoEnabled && !m_config.GoldenMode(); }

        // THE VIEWPORT'S EXTENT, from whichever target this run actually has --
        // the OffscreenCanvas' on the NVRHI arm, the offscreen graph output's
        // on the graph arm. Every phase that needs a viewport size asks HERE,
        // the same way RuntimeFrame's FrameExtent is the runtime's single
        // source of truth, so the resolver's material globals, the scene
        // camera's fit, the camera-rect overlay and the panel's Image can never
        // end up fitted to two different rectangles. 0 before the targets exist
        // (or after a failed resize), which every caller already tolerates --
        // the panel skips its Image at 0 and FrameSceneIfPending defers.
        [[nodiscard]] std::uint32_t ViewportWidth()  const noexcept;
        [[nodiscard]] std::uint32_t ViewportHeight() const noexcept;
        // The one "may editor shortcuts fire" predicate (three near-duplicates
        // collapsed): Edit mode, ImGui not capturing the keyboard, and -- for keys
        // that switch a viewport TOOL rather than act on the selection -- viewport
        // focus.
        [[nodiscard]] bool ShortcutsLive(const Arcane::InputSnapshot& snap,
                                         bool requireViewportFocus) const;

        // Play-mode dropdown (Task 6, runtime-host-fold arc): which action the
        // transport's Play button performs (see DrawSimTimeToolbar). Viewport =
        // m_play above, unchanged. SeparateWindow = LaunchStandalone (below) --
        // m_play/its toggle are never touched by that path. Persisted across
        // restarts via an ImGuiSettingsHandler ("[EditorPlayMode][State]"),
        // registered in Init beside ShaderEditorDocument::RegisterLayoutSettings;
        // a malformed or absent ini line leaves this at its Viewport default.
        Arcane::Editor::PlayLaunchMode m_playMode = Arcane::Editor::PlayLaunchMode::Viewport;

        // ImGuiSettingsHandler callbacks for m_playMode, mirroring
        // ShaderEditorDocument's layout handler (same registration site, same
        // read/write shape) -- static member functions rather than free
        // functions (like the scene/project dialog Thunks below) so they can
        // reach the private m_playMode of the instance handed through
        // handler->UserData; there is exactly one EditorApp per process.
        static void* PlayModeSettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              const char* name);
        static void  PlayModeSettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              void* entry, const char* line);
        static void  PlayModeSettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                              ImGuiTextBuffer* buf);
        void RegisterPlayModeSettings();   // called from Init, beside RegisterLayoutSettings

        // ImGuiSettingsHandler callbacks for m_panelVis ("[EditorPanels]
        // [Visibility]", one name-keyed line per hideable panel), mirroring
        // the PlayMode handler above. Registered at the same Init site.
        void RegisterPanelVisibilitySettings();
        static void* PanelVisibilitySettingsReadOpen(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     const char* name);
        static void  PanelVisibilitySettingsReadLine(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     void* entry, const char* line);
        static void  PanelVisibilitySettingsWriteAll(ImGuiContext* ctx, ImGuiSettingsHandler* handler,
                                                     ImGuiTextBuffer* buf);

        // ---- Editor layout ini (imgui.ini), per project ---------------------
        // %LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini ("default"
        // project-less). io.IniFilename BORROWS this string (ImGui never
        // copies it), so it lives here, never in a local. Retargeted at boot
        // (StageFinalize, before the first NewFrame auto-loads it) and on
        // project switch. Switch rule, on purpose: the LIVE layout follows
        // you -- each project remembers the layout you last had open in it,
        // and a project's own saved layout applies on the next BOOT into it.
        // No mid-session ini reload: ImGui applies loaded dock data only to
        // windows as they appear, so a live reload would half-apply.
        std::string m_layoutIniPath;
        void RetargetLayoutIni();

        // ---- Diagnostics dump dir, per project (GPU crash diagnostics arc,
        // Task 8; F-6 in the seam-facts survey) -----------------------------
        // Same call-site family as RetargetLayoutIni immediately above --
        // every site that retargets the layout ini also retargets where a
        // crash/hang report lands, right after, so a report from THIS
        // project always lands under THIS project's Saved/Diagnostics
        // instead of a stale one another project left behind. See
        // RetargetDumpDir's own definition (EditorApp.cpp) for the
        // <project>/Saved/Diagnostics vs exe-relative-default split.
        void RetargetDumpDir();

        // SeparateWindow's launch flow (DoLaunchStandalone, EditorAppScene.cpp)
        // is the LaunchStandalone SceneSession intent (Scene/SceneSession.hpp):
        // the toolbar's Play click issues a Request, which parks behind the
        // shared "Unsaved Scene" modal when the scene is dirty or never saved
        // and otherwise proceeds immediately, per Request's completion-convention
        // comment. The spawn effect itself lives only in DoLaunchStandalone,
        // called from RunSceneAction's LaunchStandalone case.
        //
        // Effect for the LaunchStandalone intent (RunSceneAction's case). Not
        // called directly outside that -- the SceneSession machine is the gate.
        void DoLaunchStandalone();

        // Ordered multi-select source of truth (set + primary); slice-2 consumers
        // operate on Primary(), shared by the Hierarchy panel (and, later, the
        // Inspector + viewport pick -- see SelectionContext.hpp).
        Arcane::Editor::SelectionContext m_selection;

        // Which .arcscene is being edited, whether it has unsaved changes, and the
        // one-at-a-time unsaved-changes confirm machine (SceneSession.hpp). Pure
        // state: every effect below it (dialogs, file IO, ResetRegistry) is
        // performed here.
        Arcane::Editor::SceneSession m_scene;

        // Window-menu panel visibility (PanelRegistry.hpp). The menu's
        // checkmarks and the tabs' X buttons write these same bools;
        // persisted by the [EditorPanels][Visibility] ini handler.
        Arcane::Editor::PanelVisibility m_panelVis;

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

        // Editor keybind + mouse edge tracking (architecture pass sec 6). All
        // Updated within FrameInput's phases (6a-6d) at the site each chord's
        // `down` value is computed; consumers read .pressed/.released. Replaces
        // the 17 hand-rolled m_prev* bools (undo/redo, W/E/R/Q gizmo mode,
        // Ctrl+N/O/S scene shortcuts, Ctrl+X/C/V/D clipboard shortcuts, F/Home
        // framing, LMB/RMB) that used to be scattered across this class and the
        // three functions in EditorAppFrame.cpp that consumed them.
        struct InputEdges
        {
            Edge undo, redo;            // Ctrl+Z / Ctrl+(Shift+)Z|Y
            Edge w, e, r, q;            // gizmo tools
            Edge n, o, s;               // Ctrl+N/O/S scene shortcuts
            Edge x, c, v, d;            // Ctrl+X/C/V/D clipboard shortcuts
            Edge f, home;                // camera framing
            Edge lmb, rmb;               // gizmo press/release; camera pan
        };
        InputEdges m_edges;

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

        // The EDITOR's viewport camera (Edit mode). Runtime::SetCamera is the
        // PLUGIN's seam, so a project whose game module never calls it would be
        // stuck at the identity transform -- offset (0,0), zoom 1, i.e. 1 px per
        // metre. EditorApp drives this from viewport input and pushes it into
        // the Runtime in Edit mode only; in Play the plugin's camera wins.
        // See EditorCamera.hpp for the transform convention.
        Arcane::Editor::EditorCamera m_camera;
        // RMB-drag pan gesture (rules: starts only inside the viewport, keeps
        // tracking once started -- see UpdateEditorCamera).
        struct CameraPanGesture
        {
            bool      panning = false;
            glm::vec2 lastMouse{0.0f, 0.0f};   // WINDOW px -- only the delta is used
        };
        CameraPanGesture m_camPan;
        // Point the editor camera at the selection (selectionOnly) or at the
        // whole scene. A no-op when there is nothing framable, so the user's
        // view is never thrown away by an F press that had no target.
        void FrameCamera(bool selectionOnly);
        void FrameSceneIfPending();
        // GOLDEN MODE'S REPLACEMENT FOR FrameSceneIfPending (NRI Phase 3, Task
        // 13 follow-up): re-derives the viewport camera AND the selection from
        // the scene, every frame, so the captured artifact is a pure function
        // of (scene, viewport extent, command line). See GoldenViewPin.hpp for
        // the two answers and why each is the one that is stable; see the
        // definition (EditorAppScene.cpp) for why re-applying beats a one-shot.
        // Called only under GoldenMode(); ordinary runs never reach it.
        void PinGoldenViewport();
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
        // The viewport render-target triple -- canvas, GPU picker, selection
        // outline -- created and RESIZED IN LOCKSTEP (ApplyPendingResize), plus the
        // deferred size measured last frame. Declared after m_gpu: all three hold
        // NVRHI handles and must destruct before the device. m_resolver (below)
        // holds the canvas's batcher and must destruct first -- keep this struct
        // declared BEFORE m_resolver.
        struct ViewportTargets
        {
            std::unique_ptr<Arcane::OffscreenCanvas>  canvas;
            std::unique_ptr<Arcane::PickBuffer>       pick;
            std::unique_ptr<Arcane::SelectionOutline> outline;

            // THE GRAPH ARM'S WHOLE TRIO, IN ONE OBJECT (--nri-graph, NRI
            // Phase 3, Task 8). Non-null exactly when the editor booted the
            // graph flavor; `canvas`/`pick`/`outline` are then all null and
            // vice versa -- the two arms are mutually exclusive, never mixed.
            //
            // WHY ONE MEMBER RATHER THAN THREE. The plan's "ViewportTargets
            // grows the graph trio" collapses here, and not by omission: on
            // the graph path the picker and the selection outline are NODES
            // INSIDE this same context's frame (PickNode/OutlineNode, armed
            // per frame through FrameDesc::pickOutline/pickables/selectedIds),
            // not sibling render targets with their own devices and command
            // lists. Task 9 arms them; it adds no member here.
            //
            // Offscreen mode: no window, no swapchain, one persistent
            // BGRA8_UNORM output the chrome pass samples as an ImGui image.
            // It BORROWS the NriDevice m_graphChrome owns, so it must not
            // outlive it -- guaranteed by declaration order (m_graphChrome is
            // declared near the top of this class, m_viewportTargets far
            // below, and reverse-order destruction runs this one first).
            //
            // PROJECT-SCOPED, AND THEREFORE REBUILT BY EVERY PROJECT SWITCH
            // (NRI Phase 3, Task 12) -- unlike the chrome context, which
            // survives one. Not because the OBJECT belongs to a project (its
            // two injected seams re-read CurrentProject per call and would
            // survive fine) but because its CACHES do: NriTextureCache holds
            // uploads keyed by the outgoing project's asset Guids, and
            // Batch2DNode's write-once sprite/material descriptor sets name
            // that cache's views. See EditorApp::TeardownGraphForSwitch.
            std::unique_ptr<Arcane::NriGraphContext> graph;

            std::uint32_t pendingW = 0, pendingH = 0;   // measured last frame, applied at frame top
            // body: the current EditorApp::ApplyPendingViewportResize.
            // `chrome` is the HOST-WINDOW context whose ImGuiNri caches the
            // offscreen output -- the resize contract
            // (NriGraphContext::ResizeOffscreen) requires invalidating that
            // cache BEFORE the resize, and this class owns no ImGuiNri for it.
            // Null (and unread) on the NVRHI arm.
            void ApplyPendingResize(Arcane::NriGraphContext* chrome);
        };
        ViewportTargets m_viewportTargets;
        Arcane::Editor::ViewportRect                   m_viewportRect{};
        bool                                     m_viewportActive = false;

        // Viewport-local input snapshot for the game ImGui pass, captured inside
        // FrameInput (whose locals are out of scope at the render site) and read
        // where the game UI is composited. Only what the game context needs is
        // hoisted -- the input phase's scope stays narrow.
        struct GameUiInputHandoff
        {
            glm::vec2    viewportMouse{0.0f, 0.0f};  // viewport-local cursor px
            std::uint8_t mouseButtons = 0;           // raw snap.mouseButtons (LMB=bit0)
            float        wheel        = 0.0f;        // raw snap.wheelY
            bool         inViewport   = false;
            double       frameDt      = 0.0;         // per-frame dt (seconds)
        };
        GameUiInputHandoff m_gameUi;

        // ---- The graph viewport's pick + outline arming (Task 9) ------------
        // THIS FRAME's pickable silhouettes and the hit-proxy ids the outline
        // traces, held as MEMBERS because FrameDesc borrows both as spans and a
        // span into a per-frame temporary is a dangling read the moment the
        // declaration outlives the statement. Reused rather than rebuilt, so a
        // steady-state Edit frame allocates nothing.
        //
        // Produced by CollectPickables, whose k-th entry IS hit-proxy id k+1 --
        // the SAME emitter PickBuffer::RenderIdPass uses on the NVRHI arm, so
        // the two recorders cannot assign ids differently. Empty (and unread) on
        // the NVRHI arm, which gets its ids from the PickBuffer instead.
        std::vector<Arcane::PickDrawable> m_pickDrawables;
        std::vector<std::uint32_t>        m_pickSelectedIds;

        // The graph arm's click-pick, which cannot answer in the frame it is
        // asked -- see DeferredPick.hpp for the three hazards it closes. Idle on
        // the NVRHI arm, which resolves a click synchronously inside phase 17.
        Arcane::Editor::DeferredPick m_deferredPick;

        // WHICH SCENE the entities in flight belong to. Bumped by every path
        // that REPLACES the registry's contents wholesale -- ClearSceneReferences
        // (New/Open Scene, which also stops Play) and ResetPerProjectState (a
        // project switch) -- so a readback armed against the outgoing scene
        // cannot be applied to the incoming one. Play START/STOP is covered
        // separately, by DeferredPick recording the mode alongside this: those
        // two swap the registry object without going through either site.
        //
        // A COUNTER rather than a pointer comparison: Runtime swaps the registry
        // OBJECT on Play/Stop and on plugin hot-reload, and a fresh one may land
        // on the address the previous one just vacated -- the same ABA the
        // ImGuiNri texture cache has to defend against, for the same reason.
        std::uint64_t m_sceneEpoch = 0;
        // Bumped from the two sites above; named so both read as one decision.
        void NoteSceneReplaced() noexcept { ++m_sceneEpoch; m_deferredPick.Reset(); }

        // Shader-editor services + open documents (Slice 5). The compiler is the
        // app-shared compile service: documents Submit through it, and the
        // resolver's Refresh (phase 9) Polls/Drains it once per frame, offering
        // each result to the open documents first -- that drain is the ONE place
        // compile results become NVRHI shaders.
        // Documents hold NVRHI resources -> declared after m_gpu (destruct
        // before the device) and after m_runtime (they borrow its Assets).
        std::unique_ptr<Arcane::ShaderCompiler> m_shaderCompiler;
        Arcane::ShaderSourceProvider            m_shaderSources;
        // Scene asset resolution (sprite-resolution lift, 2026-07-29): ONE
        // engine-side service owning the sprite / sprite-material / post-chain
        // caches, swept and published once per frame by RefreshSceneResolution.
        // It replaced three editor-held caches -- and the editor holding them was
        // exactly why the standalone runtime host could draw none of the three.
        // ArcaneRuntime now constructs the same object. It publishes the
        // registry's SpriteTable/SpriteMaterialTable, whose pointers are
        // non-owning, so it MUST destruct before m_runtime -- which it does,
        // being declared after it (reverse-order destruction), and before
        // m_viewportTargets, whose batcher it holds.
        // ONE trap if this ever changes: m_documents below destructs FIRST, so
        // the consumeFirst hook installed on the resolver (which reaches into
        // m_documents) is dead by the time ~SceneRenderResolver runs. That is
        // fine only because the dtor never drains -- it just un-publishes.
        std::unique_ptr<Arcane::SceneRenderResolver> m_resolver;
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

        // ---- Async file-dialog inbox (architecture pass sec 2) --------------
        // One DialogSlot per dialog kind. The old six pending strings + three
        // mutexes + the unguarded m_pendingInstanceParent sidecar live here now;
        // the instance parent rides INSIDE its payload so it is stashed under the
        // lock, taken atomically with the path, and cleared with the slot.
        struct InstanceNewResult
        {
            std::string  path;
            Arcane::Guid parent;
        };
        struct DialogInbox
        {
            DialogSlot<std::string>       sceneOpen;
            DialogSlot<std::string>       sceneSave;
            DialogSlot<std::string>       projectOpen;
            DialogSlot<std::string>       materialNew;
            DialogSlot<std::string>       materialOpen;
            DialogSlot<InstanceNewResult> instanceNew;
            void ClearAll()
            {
                sceneOpen.Clear(); sceneSave.Clear(); projectOpen.Clear();
                materialNew.Clear(); materialOpen.Clear(); instanceNew.Clear();
            }
        };
        DialogInbox m_dialogs;

        // Two shared trampolines replace the six per-dialog thunks the old
        // pending-string scheme used. SDL's dialog backend fires the callback
        // exactly once per ShowXFileDialog (null path on cancel), so the
        // heap-allocated request is single-owner and freed inside the
        // trampoline. Defined in EditorAppFrame.cpp beside the launch sites
        // they serve.
        struct PathDialogRequest
        {
            DialogSlot<std::string>* slot;
            std::uint64_t            epoch;
        };
        struct InstanceDialogRequest
        {
            DialogSlot<InstanceNewResult>* slot;
            std::uint64_t                  epoch;
            Arcane::Guid                   parent;
        };
        static void PathPickedThunk(const char* path, void* user);
        static void InstancePickedThunk(const char* path, void* user);

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

        // Problems-panel row click -> editor navigation. One switch over
        // DiagLocator::Kind; performed from the frame loop, never mid-draw.
        void RouteLocator(const Arcane::DiagLocator& locator);

        // Thin wrappers RouteLocator needs, added here rather than on
        // DocumentHost: DocumentHost only indexes documents by asset Guid
        // (its own header comment -- "open/dirty/save lifecycle over one GUID
        // asset"), so Guid->path resolution and path-based lookup are
        // EditorApp-level concerns, the same way MintOrReuseSpriteForTexture
        // above resolves through m_runtime->CurrentProject() rather than
        // living on DocumentHost.
        //
        // Resolve `guid` through the open project's registry and open/focus
        // its document (DocumentHost::OpenPath's focus-not-reopen). Null with
        // no project, an invalid guid, or an asset that does not resolve to a
        // file (mirrors MintOrReuseSpriteForTexture's failure shape).
        Arcane::Editor::EditorDocument* OpenAssetDocument(const Arcane::Guid& guid);
        // The open ShaderEditorDocument whose on-disk path equals `path`, or
        // null. ShaderEditorDocument is the only open-document type that
        // exposes a stable Path() today (Problems-panel File locator is a
        // shader-diagnostics producer only, per DiagLocator::File's callers).
        Arcane::Editor::ShaderEditorDocument* FindByPath(const std::filesystem::path& path);

        // The Arcane logo, shown at the left of the transport toolbar (Unity-style). A
        // display-referred (UNORM) texture -- NOT Assets::GetTexture's sRGB -- so it
        // composites correctly through the ImGui backend. Loaded once in Init; passed to
        // DrawSimTimeToolbar as an ImTextureID (the raw nvrhi::ITexture*). Null if the
        // image is missing (the toolbar simply omits it). Holds an NVRHI handle, so like
        // the other render resources it is declared after m_gpu (destructs before the device).
        nvrhi::TextureHandle m_toolbarLogo;
        // THE GRAPH ARM'S SAME MARK (NRI Phase 3, Task 11). The handle above
        // cannot exist without an nvrhi device, so on `--nri-graph` the logo
        // reaches the GPU the way every other image on that arm does: decoded
        // device-free (Arcane::LoadDisplayPixels, the CPU half of
        // LoadDisplayTexture) and uploaded by the CHROME context's
        // NriTextureCache under a synthetic per-run Guid -- with
        // ColorSpace::Display, because ImGui draws AFTER the tonemap and an
        // sRGB view under it decodes a second time and reads dark.
        //
        // The pixels are RETAINED (rather than decoded on demand) because the
        // cache's supply is a callback it may invoke at any Resolve, and this
        // one answers exactly one Guid.
        //
        // NO INVALIDATE OBLIGATION, unlike the viewport output, and the reason
        // is worth stating: this texture lives in the SAME context as the
        // ImGuiNri that caches it, so ~NriGraphContext's own ordering covers
        // it (nodes Release() before m_textures->Release(), i.e. the backend's
        // view is buried before the cache's texture). Cross-context is what
        // needs a caller-side hook; same-context does not.
        Arcane::Guid      m_graphLogoId;
        Arcane::PixelData m_graphLogoPixels;
        std::uint64_t     m_graphLogoTexture = 0;   // ImTextureID (raw nri::Texture*)
        // The toolbar's mark for whichever arm is live -- 0 = no mark, which
        // DrawSimTimeToolbar already treats as "skip it".
        [[nodiscard]] std::uint64_t ToolbarLogoTextureId() const noexcept
        {
            return m_graphLogoTexture != 0
                       ? m_graphLogoTexture
                       : (std::uint64_t)(std::intptr_t)m_toolbarLogo.Get();
        }

        // File -> Open Project (soft-restart). The menu sets menuReq.openProject;
        // DrawEditorUi launches the async .arcproj FILE dialog via the shared
        // PathPickedThunk trampoline, targeting m_dialogs.projectOpen; the next
        // frame's ConsumeProjectDialogResult Take()s the slot and (outside any
        // lock -- DialogSlot owns its own) runs SwitchProject at a safe point.
        // (Project::Open accepts the .arcproj file directly.)
        void SwitchProject(const std::filesystem::path& path);  // validate-then-soft-restart
        // Reset every mutable member whose value refers to the current
        // project. Called by SwitchProject's switch_teardown stage only --
        // boot has no prior project to reset. Full per-member rationale is
        // the comment block above this function's definition in
        // EditorAppProject.cpp -- that is the single owned reset list new
        // per-project members must join (architecture pass sec 3, audit
        // defect A3).
        void ResetPerProjectState();

        // The project-open SUCCESS tail (architecture pass sec 5) -- previously
        // duplicated verbatim across StageFinalize and a hand-rolled
        // switch_plugin_load stage, with a partial third copy in the switch
        // failure fallback. Now a single function called from all three sites:
        // StageFinalize, SwitchProject's plugin_load stage (which since Task 12
        // reuses the same StagePluginLoad body -- switch_plugin_load no longer
        // exists as a separate stage), and the switch failure fallback.
        // recordRecents=false is the fallback's case: it re-establishes the
        // PROJECT-LESS baseline (or the old project), and a refused open must
        // never reorder the recents lists. Adopts the current project's boot
        // scene (if m_undo and a project exist), then
        // EnsureScene/UpdateWindowTitle/NoteProjectOpened.
        // Does NOT call RetargetLayoutIni() -- every call site still owns that
        // separately, immediately after, since it is not part of the "a project
        // (or none) is now open" fact this tail represents (see each site).
        void OnProjectOpened(bool recordRecents = true);

        // ---- Build -> Rebuild Game Module (ModuleBuild.hpp) -----------------
        // StartModuleRebuild composes the premake+msbuild line for the open
        // project (against the RUNNING editor's SDK, via ARCANE_SDK) and
        // starts the worker; the menu item is greyed while playing/building/
        // module-less, and this re-checks the same gates for any future
        // caller. PollModuleBuild is the per-frame drain (EditorAppProject.cpp,
        // called from MainLoop's top-of-frame consume block): worker lines ->
        // "Build: "-prefixed Console entries; on finish, failure publishes ONE
        // Problems row under the "build:<root>" key and success clears it,
        // restamps a stale manifest abi (the module now matches this engine),
        // and -- when the module had been REFUSED at open (stale ABI), which
        // left no PluginHost watching -- re-engages the host the way
        // StagePluginLoad does. A LIVE host needs nothing here: the existing
        // debounced watcher (PluginHost::Poll, EndFrame) sees the fresh DLL
        // mtime and hot-reloads with state on its own; forcing it would race
        // the debounce.
        void StartModuleRebuild();
        void PollModuleBuild();
        Arcane::Editor::ModuleBuild::Runner m_moduleBuild;   // joined in Shutdown
        // The project root the RUNNING build was started for. Publish/Clear
        // key identity ("build:<root>") + the guard that skips restamp/
        // re-engage when the project switched mid-build.
        // Survives a project switch ON PURPOSE: it guards an async worker --
        // PollModuleBuild's root-mismatch check is the staleness guard. See
        // ResetPerProjectState's rule.
        std::filesystem::path m_moduleBuildRoot;

        // ---- Report-written notify (GPU crash diagnostics arc, Task 9) -----
        // Diagnostics::ReportWrittenHook (Diagnostics.hpp) fires on WHATEVER
        // thread wrote the report -- the hang/gpu-stall watchdog thread for
        // the two survivable kinds, or the faulting thread for a crash
        // (about to terminate; registering an asset there is moot -- there
        // is no next frame left to drain the queue into, so it simply never
        // happens, which is the honest boundary rather than a special case).
        // AssetRegistry has no lock of its own (see AssetRegistry.hpp), and
        // the Problems row's locator needs the Guid RegisterCreatedAsset
        // mints, so BOTH the registration and the Publish happen on the
        // main thread, never inside the hook itself. Same worker-writes/
        // main-drains shape as ModuleBuild::Runner (DrainLines() /
        // PollModuleBuild, above) -- OnReportWritten only pushes a path
        // into this mutex-guarded queue; PollDiagnosticReports (per-frame,
        // called beside PollModuleBuild) drains it and does the real work.
        static void OnReportWritten(const std::filesystem::path& diagPath, void* user);
        void PollDiagnosticReports();
        // Both members below are on ResetPerProjectState's reset list
        // (EditorAppProject.cpp) -- see that function's member-rationale
        // comment block for the entry. m_pendingReports because a path
        // queued against the outgoing project and drained after the switch
        // would try to RegisterCreatedAsset it into the WRONG (incoming)
        // project's registry; m_reportDiagnostics because its rows carry
        // DiagLocator::Asset(guid) values that exist only in the outgoing
        // project's registry -- surviving a switch would resurrect stale
        // rows whose click silently no-ops and whose detail line names a
        // file from a project that is no longer open (the switch-staleness
        // class ResetPerProjectState exists to institutionalize against).
        std::mutex                         m_pendingReportsMutex;
        std::vector<std::filesystem::path> m_pendingReports;      // guarded by m_pendingReportsMutex
        // KEY OWNERSHIP: "diagnostics:reports" -- accumulated across the
        // CURRENT project's session (one row per report successfully
        // registered against it), so an earlier report's row survives a
        // later report's Publish (publication-group replace semantics --
        // Diagnostics.hpp) UNTIL a project switch resets it. Main-thread
        // only, like the queue above -- PollDiagnosticReports is the only
        // writer.
        std::vector<Arcane::Diagnostic>    m_reportDiagnostics;

#if !defined(ARCANE_DIST)
        // ---- Deliberate GPU fault (GPU crash diagnostics arc, Task 11) ------
        // The desk battery's trigger: Build -> Diagnostics -> Crash GPU
        // (diagnostics test) raises MenuRequests::crashGpu, ConsumeMenuRequests
        // calls this, and everything downstream of the dispatch is the arc's own
        // capture path with nothing special about having been asked for.
        //
        // Built LAZILY at the first click rather than at boot: a compute
        // pipeline plus a UAV that exist in every session only to be used in
        // approximately none of them is a cost with no reader.
        //
        // NOT on ResetPerProjectState's list, on purpose: it holds DEVICE
        // resources, not project ones, and the device outlives a project switch
        // -- the switch-staleness rule that governs m_reportDiagnostics above
        // does not reach it.
        //
        // Declared after m_gpu (top of the member block) so it releases its
        // NVRHI handles before the device that made them.
        void FireDeliberateGpuFault();
        std::unique_ptr<Arcane::GpuFaultInjector> m_gpuFault;
        // --crash-gpu N fired already. The menu item is deliberately NOT
        // latched by this -- a user who clicks twice meant it twice; the latch
        // exists only so a per-frame schedule fires once.
        bool                                      m_gpuFaultFired = false;
#endif

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
        // pushes m_modalErrors with the reason under a "Scene Error" title
        // (DrawModals draws the queue's front as a modal).
        bool DoNewScene();
        bool DoOpenScene(const std::filesystem::path& file);
        bool DoSaveScene(const std::filesystem::path& file);

        // Write <projectRoot>/Saved/AutoScreenshot.png from the viewport
        // output -- the editor half of the Arcane Hub's cover-thumbnail
        // chain (Unreal's model). Called on scene save and on clean
        // shutdown; a no-op without a project, a viewport, or a device.
        void WriteAutoScreenshot();
        // The graph arm's half of it (NRI Phase 3, Task 11): one extra
        // VIEWPORT frame with FrameDesc::capture armed, then ReadCapture into
        // the shared thumbnail writer. NEVER the chrome context -- an editor
        // cover is a picture of the scene, and see PresentChromeFrame's
        // absence table for why a chrome capture would redefine a golden.
        //
        // NOT #if-guarded, for the same reason m_graphChrome is not (see its
        // declaration): a preprocessor-guarded member forces a guard at every
        // use site. In Dist it is simply unreachable -- CreateGraphVehicles'
        // whole body is guarded, so m_viewportTargets.graph is always null.
        bool CaptureGraphViewportPng(const std::filesystem::path& file);

        // Window title, recomposed from project + scene + dirty state each frame
        // and pushed to the window only when it changes (SetTitle is an SDL call,
        // and the string is identical on the overwhelming majority of frames).
        std::string m_windowTitle;
        void        UpdateWindowTitle();

        // ---- File -> Open Recent / Open Recent Scene ------------------------
        // Both recents lists live behind one facade (EditorRecents.hpp,
        // architecture pass sec 10, B8): the Hub's shared machine-wide project
        // list (m_recents.projects, RecentProjects.hpp) and the PER-PROJECT
        // scene list (m_recents.scenes, <root>/Saved/recent_scenes.json,
        // SceneRecents.hpp) were two fully parallel ~10-site call ladders,
        // always invoked in adjacent lines -- these used to be separate
        // members (m_recents/m_fileMenuWasOpen/m_sceneRecents) with four
        // methods between them.
        //
        // Cached rather than rebuilt per frame: building the project list
        // reads a file and stats every candidate, and the menu bar draws
        // every frame whether or not File is ever opened. Both lists are
        // refreshed at exactly two moments -- a successful project open
        // (NoteProjectOpened) and the frame the File menu is first opened
        // (RefreshAll, m_recents.fileMenuWasOpen tracks the rising edge). The
        // scene list is pushed ONLY from the two scene success paths --
        // DoOpenScene and DoSaveScene, after m_scene.Adopt -- so a refused
        // open or a failed save never reorders or pollutes either list.
        Arcane::Editor::EditorRecents m_recents;

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
        // Survives a project switch ON PURPOSE: re-resolved from the (now
        // empty) DocumentHost next frame -- the ResolveActiveMaterialDoc
        // fallback self-heals it. See ResetPerProjectState's rule.
        Arcane::Guid m_activeMaterialGuid;
        // Open material-document count LAST frame, so "the last one closed" is
        // a detectable edge even when the Viewport does not report Appearing
        // (a document that was floating rather than tabbed over the scene).
        // Survives a project switch ON PURPOSE: re-resolved from the (now
        // empty) DocumentHost next frame -- the ResolveActiveMaterialDoc
        // fallback self-heals it. See ResetPerProjectState's rule.
        std::size_t  m_materialDocCount = 0;

        // Failure surfacing for project-open, scene, and standalone-launch
        // refusals (architecture pass sec 7): SwitchProject's refusals used to
        // be console-only and were repeatedly missed at the desk, and the
        // scene/launch effects had their own parallel string + popup each.
        // Any refusal/failure Push()es a titled error here; DrawModals draws
        // the queue's front as a blocking modal (main thread only -- pushed
        // inside the effects/SwitchProject/Init, read in the ImGui pass; no
        // lock needed). Errors display one at a time, FIFO, instead of racing
        // for the popup stack.
        Arcane::Editor::ModalErrorQueue m_modalErrors;

        // One-shot latch for RaiseOpenProjectOnStart: consumed on the first frame
        // that draws the menu bar, so the picker appears over a live editor window
        // rather than before one exists.
        bool m_raiseOpenProjectOnStart = false;

        // Set by SwitchProject when its overlay BootPresenter reports the window
        // closed mid-switch (BootResult::quitRequested), instead of treating that
        // as a failed switch (Important 1, 2026-07-31 review): the presenter
        // already consumed the OS quit event during its own event pump, so it
        // will not reach PumpFrameEvents' own SDL_EVENT_QUIT check on a later
        // frame. PumpFrameEvents reads this flag first and requests the same
        // normal exit a live quit would, instead of surfacing through m_modalErrors.
        // Safe to check unconditionally: SwitchProject's switch_teardown stage
        // has already reset m_scene to a clean Untitled state by the time this
        // can be set, so there is nothing for the unsaved-changes confirm modal
        // to protect -- an immediate exit is correct, not just convenient.
        bool m_requestExit = false;
    };
}
