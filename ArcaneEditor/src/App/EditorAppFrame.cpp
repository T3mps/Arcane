// EditorApp, frame loop: MainLoop plus the per-frame phase methods it calls.
// Split out of EditorApp.cpp as a pure move -- every statement kept its
// position relative to every other.
//
// MainLoop is a straight sequence of the phase calls, IN THE ORDER THE ORIGINAL
// STATEMENTS RAN. That order is load-bearing: the render pass has to precede
// the ImGui pass that samples its texture, the camera has to be pushed twice,
// the deferred dialog results have to be consumed before any new request is
// issued, and so on. None of it is covered by a test -- this TU is not compiled
// into ArcaneTests -- so each phase below carries the constraint it
// participates in, and the phase DEFINITIONS are kept in frame order so the
// "above"/"below" locators in their comments stay true.

#include "App/EditorApp.hpp"
#include "Panels/EditorPanels.hpp"
#include "Scene/SelectionOps.hpp"
#include "Viewport/ViewportImGuiInput.hpp"

#include <Arcane/Assets/Assets.hpp>      // Arcane::WritePngRgba (RenderSceneToViewport's graph-arm capture); ReadTexturePixels was CaptureEditorGolden's, deleted with the NVRHI arm at NRI Phase 5a, Task 4
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat -- the hang watchdog's liveness signal
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Host/GoldenHarness.hpp>   // Arcane::DrainSceneCompiles/GoldenArtifact/kEditorGoldenNamePrefix (NRI Phase 3, Task 13)
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved -- the device-loss latch
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (gizmo drag target)
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (Play view + camera rect)
#include <Arcane/Scene/TransformSystems.hpp>   // Edit-mode derived-transform refresh
#include <Arcane/Serialization/SceneAsset.hpp>   // Arcane::Scene::kSceneExt (Save-dialog suffix)

#include <Astra/Registry/Registry.hpp>   // Registry::InspectEntity/GetComponent (gizmo descriptor resolve)

#include <glm/glm.hpp>

#include <imgui.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (Assets -> Show in Explorer)
#endif

namespace Arcane::Editor
{
    namespace
    {
        // Undo/redo keybind scancodes (see FrameInput's shortcut phase). Hoisted to
        // file scope -- pure cleanup, values unchanged (verified against SDL_scancode.h).
        constexpr uint32_t kScLCtrl  = 224;  // SDL_SCANCODE_LCTRL
        constexpr uint32_t kScRCtrl  = 228;  // SDL_SCANCODE_RCTRL
        constexpr uint32_t kScLShift = 225;  // SDL_SCANCODE_LSHIFT
        constexpr uint32_t kScRShift = 229;  // SDL_SCANCODE_RSHIFT
        constexpr uint32_t kScY      = 28;   // SDL_SCANCODE_Y
        constexpr uint32_t kScZ      = 29;   // SDL_SCANCODE_Z

        // Gizmo mode-switch keybind scancodes (see HandleGizmoModeKeys). No
        // conflict with the Sandbox plugin's camera (RMB-pan + wheel-zoom only).
        constexpr uint32_t kScW = 26;   // SDL_SCANCODE_W
        constexpr uint32_t kScE = 8;    // SDL_SCANCODE_E
        constexpr uint32_t kScR = 21;   // SDL_SCANCODE_R
        constexpr uint32_t kScQ = 20;   // SDL_SCANCODE_Q

        // File-menu scene shortcuts: Ctrl+N / Ctrl+O / Ctrl+S (see
        // HandleUndoRedoAndSceneShortcuts). The menu advertises these, so they are
        // handled rather than drawn.
        constexpr uint32_t kScN = 17;   // SDL_SCANCODE_N
        constexpr uint32_t kScO = 18;   // SDL_SCANCODE_O
        constexpr uint32_t kScS = 22;   // SDL_SCANCODE_S

        // Edit-menu clipboard shortcuts: Ctrl+X/C/V/D (see
        // HandleUndoRedoAndSceneShortcuts). Same table as kScY/kScZ above.
        constexpr uint32_t kScX = 27;   // SDL_SCANCODE_X
        constexpr uint32_t kScC = 6;    // SDL_SCANCODE_C
        constexpr uint32_t kScV = 25;   // SDL_SCANCODE_V
        constexpr uint32_t kScD = 7;    // SDL_SCANCODE_D

        // Viewport camera framing keys (see UpdateEditorCamera): F frames
        // the selection, Home frames the whole scene.
        constexpr uint32_t kScF    = 9;    // SDL_SCANCODE_F
        constexpr uint32_t kScHome = 74;   // SDL_SCANCODE_HOME

        // ASCII-lowercased extension, for a case-insensitive suffix check: a
        // hand-typed "MyScene.ARCSCENE" already names an .arcscene, and stapling a
        // second suffix onto it produces a file the user did not ask for.
        std::string LowerExtension(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (char& c : ext)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return ext;
        }

        // Resolve the ComponentDescriptor for Transform on `e`, for bracketing
        // a gizmo drag into the undo stack via CommandStack::SnapshotComponent.
        // Mirrors EditorPanels.cpp's Inspector loop (InspectEntity + meta->typeName
        // match) -- namespace-qualified, matching how ASTRA_REFLECT_TYPE registers it.
        const Astra::ComponentDescriptor* FindTransformDescriptor(Astra::Registry& reg, Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            {
                if (ci.meta && ci.meta->typeName == "Arcane::Transform")
                {
                    return ci.descriptor;
                }
            }
            return nullptr;
        }

        // Assets -> Show in Explorer / Copy Path, on a resolved asset Guid.
        // Extracted so the menu-bar route (the browser's tracked row) and the
        // Asset Browser's own row context menu (AssetBrowser.cpp) resolve the
        // SAME way -- one implementation, two entry points.
        void AssetPathAction(const Arcane::Project* proj, const Arcane::Guid& guid,
                             bool showInExplorer, bool copyPath)
        {
            const auto assetPath = proj
                ? proj->ResolveAsset(Arcane::AssetId::FromGuid(guid))
                : std::nullopt;
            if (!assetPath)
            {
                ARC_WARN("Assets: the selected asset no longer resolves to a file");
                return;
            }
            if (showInExplorer)
            {
                // explorer /select opens the folder WITH the file focused.
                const std::wstring args = L"/select,\"" + assetPath->wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe",
                              args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (copyPath)
                ImGui::SetClipboardText(assetPath->string().c_str());
        }
    }

    // Two shared file-dialog completion trampolines (declared in EditorApp.hpp),
    // replacing the six per-dialog thunks the old pending-string scheme used.
    // SDL's dialog backend fires the callback exactly once per ShowXFileDialog
    // (null path on cancel -- see the early return below), so the heap-allocated
    // request is single-owner and freed here. If a future backend ever skipped
    // the callback the request would leak (bounded, one small struct per
    // un-fired dialog) -- acceptable, noted.
    void EditorApp::PathPickedThunk(const char* path, void* user)
    {
        std::unique_ptr<PathDialogRequest> req(static_cast<PathDialogRequest*>(user));
        if (path)
            req->slot->Stash(req->epoch, path);
    }

    void EditorApp::InstancePickedThunk(const char* path, void* user)
    {
        std::unique_ptr<InstanceDialogRequest> req(static_cast<InstanceDialogRequest*>(user));
        if (path)
            req->slot->Stash(req->epoch, InstanceNewResult{ path, req->parent });
    }

    // The frame. Every line here is a phase call, and the ORDER IS THE
    // BEHAVIOUR -- see this file's header comment and each phase's own note.
    void EditorApp::MainLoop()
    {
        LoopState ls;
        ls.simPrev       = std::chrono::steady_clock::now();
        ls.lastFrameTime = ls.simPrev;

        // ===== THE GOLDEN WARM-UP (NRI Phase 3, Task 13) =====================
        // The editor's counterpart of RuntimeApp::MainLoop's warm-up block,
        // reusing Arcane::DrainSceneCompiles VERBATIM (Host/GoldenHarness.hpp)
        // -- see that function's own comment for the failure it closes: a
        // pinned frame clock (below, FrameInput/AdvanceSim) makes Time a pure
        // function of the frame index, but does NOT pin what the frame
        // CONTAINS, and a golden captured before the scene's materials have
        // bound would freeze an unshaded sprite as the baseline.
        //
        // Runs BEFORE the while loop -- zero frames render on a warm-up
        // failure, exactly like the runtime's early return, rather than
        // letting the loop start and asking a later phase to notice. m_resolver
        // is already built by here (StageSpriteTables, a boot stage that ran
        // before Main()/MainLoop()); ViewportWidth/Height read whatever extent
        // the viewport targets have at boot (1280x720 by default -- see
        // BuildGraphViewportContext), the same "boot-time extent" the runtime
        // warm-up uses before its own first frame.
        if (m_config.GoldenMode() && m_resolver)
        {
            Arcane::Diagnostics::SetPhase("golden compile warm-up");
            const float warmupW = (float)ViewportWidth();
            const float warmupH = (float)ViewportHeight();
            if (!Arcane::DrainSceneCompiles(*m_resolver, *m_shaderCompiler, warmupW, warmupH))
            {
                ARC_ERROR("golden: shader compiles did not settle within {:.0f}s -- refusing to "
                          "capture or compare a frame whose content is not bound yet",
                          Arcane::kGoldenWarmupTimeoutSeconds);
                m_goldenExit = 3;
                return;
            }

            // Quiescent is not the same as complete: a compile that FAILED
            // also leaves the service idle. Same census RuntimeApp::MainLoop
            // reads. `postBound` used to split between the NVRHI and graph
            // recorders (the graph arm's post chain is bytes with no NVRHI
            // object to ask Ready() of); NRI Phase 5a, Task 4 deleted that
            // NVRHI object and rebuilt census.postBound's own computation on
            // the same bytes the graph arm already read (see
            // SceneRenderResolver.cpp's Materials()), so both sides of the
            // ternary this used to be agreed. Task 11a collapsed it to the
            // surviving arm: PostDesc() read directly.
            const Arcane::SceneRenderResolver::MaterialCensus census = m_resolver->Materials();
            const bool postBound = (m_resolver->PostDesc() != nullptr);
            if (census.spriteBound != census.spriteReferenced ||
                (census.postReferenced && !postBound))
            {
                ARC_ERROR("golden: the scene's materials did not all bind ({}/{} sprite material(s), "
                          "post chain {}) -- refusing rather than freezing an incomplete frame as "
                          "the baseline; the compile failures are logged above",
                          census.spriteBound, census.spriteReferenced,
                          census.postReferenced ? (postBound ? "bound" : "UNBOUND") : "none");
                m_goldenExit = 3;
                return;
            }
            ARC_INFO("golden: scene materials settled before frame 1 -- {} sprite material(s), "
                     "post chain {}", census.spriteBound,
                     census.postReferenced ? "bound" : "none");
        }

        // Boot is over; anything the watchdog reports from here on belongs to
        // the frame loop, not to a stale boot stage.
        Arcane::Diagnostics::SetPhase("editor frame loop");

        while (ls.running)
        {
            // FIRST statement in the frame, before any phase can block. The
            // watchdog's whole definition of "alive" is this call, so it has to
            // sit on the loop itself rather than inside any one phase.
            Arcane::Diagnostics::Heartbeat();

            // Device-lost exit: the latch is set only AFTER the gpu-crash
            // report was written, so breaking here is "report captured, stop
            // cleanly". Checked at the frame top so no phase downstream ever
            // records against the dead device -- the next resource-creating
            // path (PickBuffer's internal command list was the desk repro)
            // is an access violation inside nvrhi, not an error code.
            // Mirrors RuntimeApp::MainLoop.
            if (Arcane::GpuDeviceLostObserved())
            {
                ARC_ERROR("GPU device lost -- crash report written; shutting down");
                break;
            }

            const FramePump pump = PumpFrameEvents();
            if (pump == FramePump::Exit)
            {
                break;
            }
            if (pump == FramePump::SkipFrame)
            {
                continue;
            }

            ConsumeDeferredSceneAction(ls);
            ConsumeSceneDialogResults(ls);
            ConsumeProjectDialogResult();
            ConsumeMaterialDialogResults();
            // Worker -> main-thread drain of the module rebuild, at the same
            // safe point the dialog results land at: its finish path can run
            // effects (plugin re-engage, scene reload) that must never land
            // mid-render. See EditorApp.hpp's Build section.
            PollModuleBuild();
            // Watchdog/faulting-thread -> main-thread drain of the report-
            // written queue, same safe point: registers each new .arcdiag
            // as an asset and publishes one Problems row per report. See
            // EditorApp.hpp's Report-written notify section.
            PollDiagnosticReports();

#if !defined(ARCANE_DIST)
            // --crash-gpu N: the same deliberate fault Build -> Diagnostics ->
            // Crash GPU fires, on a schedule, so the desk battery's editor items
            // can be SCRIPTED rather than clicked -- and so a flag both hosts
            // parse is not silently inert in one of them. Here rather than at
            // the menu-request site because this is the frame's provably safe
            // point: FireDeliberateGpuFault dispatches through
            // NriDiagnostics::FireFault on its own command buffer, and this
            // top-of-frame point is safe because no graph frame has been
            // declared yet. Fires exactly once.
            if (m_config.crashGpuFrame != 0 && !m_gpuFaultFired &&
                m_frameCount >= m_config.crashGpuFrame)
            {
                m_gpuFaultFired = true;   // set FIRST: a failed build must not retry every frame
                FireDeliberateGpuFault();
            }
#endif

            FrameState fs;
            FrameInput(ls, fs);
            AdvanceSim(ls);
            ApplyPendingViewportResize();
            RefreshSceneResolution();
            RenderSceneToViewport();
            CompositeGameUi();
            RenderSelectionOutline();
            // NRI Phase 3, Task 13: used to be the NVRHI arm's golden capture,
            // sited here because the canvas output texture had its final
            // content for this frame by this point (CompositeGameUi/
            // RenderSelectionOutline were its last writers) and
            // PumpEditorDocuments/DrawEditorUi below touch only the editor's
            // OWN ImGui frame, never the viewport texture. NRI Phase 5a, Task
            // 4 deleted that arm along with the canvas it read -- see
            // CaptureEditorGolden's own comment for why the call stays here
            // as a standing no-op rather than being removed.
            CaptureEditorGolden();
            PumpEditorDocuments();
            DrawEditorUi(ls, fs);
            DrawModals(ls);
            DrawViewportPanelPhase(fs);
            SyncCenterTabFocus(fs);
            HandleViewportPick(fs);
            DrawSelectionPanels();
            if (!PresentFrame())
            {
                continue;
            }
            EndFrame(ls);
        }

        // NRI Phase 3, Task 13 fix round 1 (IMPORTANT, review finding 2): a
        // whole-run property, checked once here rather than per-frame --
        // "did this golden run ever actually capture a frame". Without it, a
        // run whose every attempt landed on a Skipped viewport frame (graph
        // arm, a collapsed/zero-sized panel) or a null canvas
        // (NVRHI arm, a lost viewport context) would reach maxFrames with
        // m_goldenExit still 0 and Run() would report a clean PASS having
        // compared NOTHING. Reachable with no code change on this task's
        // part: the Viewport panel's docked/visible state is imgui.ini
        // layout state persisted per project GUID (see RetargetLayoutIni),
        // so a saved layout with the panel's tab closed silently produces
        // exactly this shape. m_goldenCaptured is set by BOTH capture paths
        // (CaptureEditorGolden, RenderSceneToViewport's capture block) the
        // moment either confirms a genuine attempt against a real target --
        // see their own comments. Skipped when GoldenMode() is off (the
        // member stays false and unread on every ordinary run) and after a
        // warm-up refusal (that path already returns before the loop, with
        // m_goldenExit already 3).
        if (m_config.GoldenMode() && !m_goldenCaptured)
        {
            ARC_ERROR("golden: the run finished without ever capturing a frame to compare or "
                      "write -- the viewport had nothing to capture on every attempt (a collapsed "
                      "panel, a lost viewport context, or the run ended before reaching one)");
            m_goldenExit = 3;
        }
    }

    // Phase 1: pump the window/OS event queue. Must stay first -- everything
    // below reads the window size and the quit/minimize state it settles.
    EditorApp::FramePump EditorApp::PumpFrameEvents()
    {
        // A project-switch overlay presenter already saw and consumed the OS
        // quit event on its own pump (SwitchProject, EditorAppProject.cpp) --
        // it will never reach the SDL_EVENT_QUIT check below. m_requestExit is
        // how that failure branch hands the exit back to the normal frame loop
        // instead of reporting it as a switch failure (Important 1, 2026-07-31
        // review). The scene is already clean at that point (switch_teardown
        // reset it), so an immediate exit is correct -- no confirm modal.
        if (m_requestExit)
            return FramePump::Exit;

        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested)
        {
            // Unsaved scene: park the quit behind the confirm modal rather than
            // dropping the user's work on a window close. Request returns false
            // when it parks, and this frame then runs normally so the modal draws.
            if (m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
            {
                // Exiting here returns 0 from Run(), which is indistinguishable
                // from a normal shutdown in the process exit code -- so an
                // unwanted quit looks like the editor simply vanished. Say it.
                ARC_INFO("Editor: exiting on a quit request (see the Window line above for its source)");
                return FramePump::Exit;
            }
        }
        // ONE resize, to whichever presentation surface this run has, and
        // strictly BETWEEN frames -- which the top of the loop structurally is
        // (RuntimeFrame::PumpAndResize carries the same pair for the same
        // reason). The CHROME context's swapchain is the surface bound to
        // this window. The VIEWPORT's offscreen output is NOT resized here at
        // all -- it tracks the panel, not the window, and phase 8 owns it.
        // The `else` arm that used to call GpuContext::OnResize (an NVRHI
        // swapchain resize -- the graph flavor never created one) is deleted
        // outright now (NRI Phase 5a, Task 6): OnResize itself is gone, along
        // with the rest of GpuContext's NVRHI half.
        if (events.resized)
        {
            if (m_graphChrome)
                m_graphChrome->Resize(events.width, events.height);
        }
        if (m_gpu->Win().IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return FramePump::SkipFrame;
        }
        return FramePump::Continue;
    }

    // Perform a scene intent SceneSession parked (or one that needed no
    // confirmation). Only ever called from the top-of-frame phases below -- see
    // sceneAction.
    void EditorApp::RunSceneAction(const Arcane::Editor::SceneSession::PendingRequest& req, LoopState& ls)
    {
        switch (req.intent)
        {
            case Arcane::Editor::SceneIntent::NewScene:
                DoNewScene();
                break;
            case Arcane::Editor::SceneIntent::OpenScene:
                DoOpenScene(req.path);
                break;
            case Arcane::Editor::SceneIntent::OpenProject:
                SwitchProject(req.path);
                break;
            case Arcane::Editor::SceneIntent::Exit:
                ls.running = false;
                break;
            case Arcane::Editor::SceneIntent::LaunchStandalone:
                DoLaunchStandalone();
                break;
            case Arcane::Editor::SceneIntent::None:
                break;
        }
    }

    // Phase 2: last frame's confirm-modal answer. MUST run before any phase that
    // can issue a new SceneSession::Request -- the session parks only one intent
    // at a time.
    void EditorApp::ConsumeDeferredSceneAction(LoopState& ls)
    {
        // The confirm modal's answer from LAST frame, run here at the same safe
        // point the project switch uses. Taken before any new Request below: the
        // session allows only one parked intent at a time.
        if (ls.sceneAction.intent != Arcane::Editor::SceneIntent::None)
        {
            const Arcane::Editor::SceneSession::PendingRequest req = ls.sceneAction;
            ls.sceneAction = {};
            RunSceneAction(req, ls);
        }
    }

    // Phase 3: scene file-dialog results. The thunks Stash() into the slots on
    // an SDL BACKGROUND thread; this is the main-thread Take() half and must
    // stay at the top of the frame, never mid-render.
    void EditorApp::ConsumeSceneDialogResults(LoopState& ls)
    {
        if (const auto sceneOpen = m_dialogs.sceneOpen.Take())
        {
            // Guarded: opening over unsaved work parks the intent for the confirm
            // modal (drawn later this frame) instead of discarding it.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene, *sceneOpen, *m_undo))
            {
                DoOpenScene(*sceneOpen);
            }
        }
        if (const auto sceneSave = m_dialogs.sceneSave.Take())
        {
            std::filesystem::path p = *sceneSave;
            // The save dialog does not force the extension (Window.hpp), and a
            // scene without it does not scan as an asset. Case-insensitive:
            // a hand-typed "MyScene.ARCSCENE" must not become
            // "MyScene.ARCSCENE.arcscene".
            if (LowerExtension(p) != Arcane::Scene::kSceneExt)
            {
                p += Arcane::Scene::kSceneExt;
            }
            const bool saved = DoSaveScene(p);

            // This save may be the confirm modal's "Save" answer on a never-saved
            // scene: that branch launches this dialog and leaves the intent parked
            // until the path lands, which is now. Proceed only if the bytes
            // actually went to disk -- a failed save must not go on to discard the
            // work it was meant to preserve.
            if (m_scene.Pending() != Arcane::Editor::SceneIntent::None)
            {
                if (saved)
                {
                    RunSceneAction(m_scene.TakePending(), ls);
                }
                else
                {
                    m_scene.ClearPending();
                }
            }
        }
    }

    // Phase 4: File->Open Project result. Same epoch-guarded slot pattern;
    // the soft restart must land at the top of the frame, never mid-render.
    void EditorApp::ConsumeProjectDialogResult()
    {
        // File->Open Project: run a pending soft-restart at a safe point (top of
        // frame, never mid-render). Stashed by PathPickedThunk, which runs on an
        // SDL-owned background thread -- Take() the path out of the slot, then
        // switch on the local copy (SwitchProject itself never touches m_dialogs).
        if (const auto pending = m_dialogs.projectOpen.Take())
        {
            // Same guard as Open Scene: the outgoing project's scene may have
            // unsaved changes, and switching would drop them.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenProject, *pending, *m_undo))
            {
                SwitchProject(*pending);
            }
        }
    }

    // Phase 5: material/instance file-dialog results. Same epoch-guarded slot
    // pattern; consumed here so document creation never lands mid-render.
    void EditorApp::ConsumeMaterialDialogResults()
    {
        if (const auto materialNew = m_dialogs.materialNew.Take())
        {
            CreateMaterialAt(*materialNew);
        }
        if (const auto materialOpen = m_dialogs.materialOpen.Take())
        {
            m_documents.OpenPath(*materialOpen);
        }
        if (const auto instance = m_dialogs.instanceNew.Take())
        {
            CreateInstanceAt(instance->path, instance->parent);
        }
    }

    // Phase 6: input sample + the editor's own keybinds + gizmo interaction.
    // Input sample (before ImGui BeginFrame so capture flags are set).
    // Runs before the sim advance, and the camera push at its tail is the FIRST
    // of this frame's two -- the gizmo hit-test below it reads Runtime::
    // CameraOffset/CameraZoom.
    void EditorApp::FrameInput(LoopState& ls, FrameState& fs)
    {
        // Cleared here, set below if a gizmo drag starts or ends THIS frame --
        // the click-pick block (later, after ImGui builds the Viewport panel)
        // checks it to avoid treating a gizmo-handle click as a selection change.
        m_gizmoCapturedClick = false;

        // Play/edit authority (architecture pass sec 1): re-derived HERE -- before
        // any phase that reads it -- and again immediately after the toolbar draw
        // in DrawEditorUi, the one site that can START Play mid-frame. RULE: every
        // site that can call PlayMode Start/Stop within the frame must be followed
        // by this same re-derivation. (A mid-frame STOP elsewhere leaves edit
        // affordances off for the rest of that frame -- the safe direction; a
        // mid-frame START without re-derivation is the A1 memento-vs-play-registry
        // bug this write exists to kill.)
        m_editBinding.editMode = !InPlayMode();

        const auto now = std::chrono::steady_clock::now();
        const double wallDt = std::chrono::duration<double>(now - ls.lastFrameTime).count();
        ls.lastFrameTime = now;
        // NRI Phase 3, Task 13: golden runs pin the frame clock too, before
        // m_gameUi.frameDt/m_editorClock (RefreshSceneResolution) consume it
        // -- the same pin RuntimeFrame::AdvanceSim applies to its own wall-dt
        // read (RuntimeFrame.cpp:227) and AdvanceSim below applies to simDt,
        // for the identical reason: a golden run must make every animated
        // shader input a pure function of the frame index, not of how fast
        // this desk happens to be running. Split across two phase functions
        // here (FrameInput's wall dt, AdvanceSim's sim dt) rather than one --
        // the editor's frame is not laid out the way RuntimeApp::MainLoop's
        // was before Task 4 extracted it -- so both need the identical pin;
        // see AdvanceSim's own comment for its half.
        const double frameDt = m_config.GoldenMode() ? 1.0 / 60.0 : wallDt;
        const Arcane::InputSnapshot snap = m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(), m_gpu->Imgui().WantCaptureMouse());

        // The plugin only sees scene-relevant input when the Viewport panel
        // is active (hovered/focused), with the cursor remapped into
        // viewport-local pixels (m_viewportRect/m_viewportActive are set at
        // the end of the PREVIOUS frame's ImGui pass -- see below). Otherwise
        // zero the mouse buttons/scroll so the plugin's camera does not pan
        // and spawn/drag does not fire while editing panels.
        Arcane::InputSnapshot pluginSnap = snap;
        float lx = 0, ly = 0;
        const bool inViewport = m_viewportActive && Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, lx, ly);
        if (inViewport)
        {
            pluginSnap.mouseX = lx;      // plugin camera works in viewport-local px
            pluginSnap.mouseY = ly;      // (panel size == offscreen size => scale 1)
            // The Viewport panel IS the world surface: from the scene's point of
            // view the cursor is in the world, not over a HUD widget. The sampled
            // wantCaptureMouse is true (the cursor is over an ImGui window -- the
            // Viewport), which the plugin uses to SUPPRESS camera pan/zoom + world
            // interaction. Clear it so RMB-pan and wheel-zoom work in the viewport.
            // (When the plugin's own HUD floats over the viewport and is hovered,
            // ImGui's hover z-order makes the Viewport window not-hovered ->
            // inViewport is false -> the suppression below still applies.)
            pluginSnap.wantCaptureMouse = false;
        }
        else
        {
            pluginSnap.mouseButtons = 0;
            pluginSnap.wheelY       = 0.0f;
        }

        // The game's viewport debug UI (its own ImGui context, composited into
        // the viewport texture -- see CompositeGameUi) sits over the
        // scene, so a click on one of its widgets must claim the pointer ahead
        // of both the plugin's gameplay input and the editor gizmo/click-pick.
        // Uses the game context's LAST-frame WantCaptureMouse: this frame's
        // game ImGui pass runs later (after this input phase closes -- see
        // CompositeGameUi), so its capture state is not known yet this frame. That
        // 1-frame lag matches the one already inherent to inViewport/
        // m_viewportActive (both computed from the previous frame's panel
        // hover/focus).
        //
        // UNCONDITIONAL AGAIN ON BOTH ARMS (NRI Phase 3, Task 9). Task 8 read
        // this as `m_gameImgui ? ... : false` because the graph arm had no game
        // context at all and nothing composited a HUD over that viewport, so
        // nothing could claim the pointer. Both halves of that are gone: the
        // graph arm now builds the same OffscreenImGuiLayer (device-less
        // flavor) and composites its draw data through a graph node, so its
        // WantCaptureMouse is as real as the NVRHI arm's. Restoring this line
        // is one third of the single change that also restored the plugin's own
        // ImGui context (StageRenderBridge) and let DrawUIAll run on this arm.
        fs.gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
            InPlayMode(), inViewport, m_gameImgui->WantCaptureMouse());

        // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
        // game ImGui pass, which composites into the viewport AFTER this input
        // phase returns (see CompositeGameUi). Only the few values the game
        // context needs are hoisted -- the input phase stays narrow. Off the
        // viewport, hasInput is false so the game context reads the cursor as
        // off-target (BeginFrame injects -FLT_MAX).
        m_gameUi.viewportMouse = glm::vec2(lx, ly);
        m_gameUi.inViewport    = inViewport;
        m_gameUi.mouseButtons  = snap.mouseButtons;
        m_gameUi.wheel         = snap.wheelY;
        m_gameUi.frameDt       = frameDt;

        // Edit mode: the editor owns the left mouse button in the viewport
        // (click-pick + gizmo), so the hosted plugin must not also see it --
        // otherwise its LMB interactions (e.g. Sandbox spawn/drag/throw) fire
        // while editing. RMB + wheel stay live so plugin camera pan/zoom still
        // navigates the scene. (LMB=bit0; InputSnapshot.hpp.) ALSO clear it
        // when the game's viewport debug UI claims the pointer this frame
        // (Play + cursor over a game HUD widget) -- otherwise a HUD click
        // would fall through and spawn/drag gameplay underneath it.
        if (!InPlayMode() || fs.gameUiClaims)
        {
            pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);
        }
        // Edit mode: the EDITOR camera owns RMB-drag pan and wheel zoom
        // (see UpdateEditorCamera below), so the plugin must not see those
        // either. Both cameras write the SAME Runtime slot, and the
        // editor's push (before SubmitRender) wins in Edit mode -- so a
        // plugin still panning underneath would just be invisible work
        // whose result reappears the moment Play starts, from a viewpoint
        // the user never chose. RMB=bit1; InputSnapshot.hpp. Play is
        // untouched: the plugin keeps RMB + wheel and its camera wins.
        if (!InPlayMode())
        {
            pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x2u);
            pluginSnap.wheelY = 0.0f;
        }
        m_runtime->SetInputSnapshot(pluginSnap);
        m_gpu->Input().Update(frameDt, snap);

        HandleUndoRedoAndSceneShortcuts(snap, fs);
        HandleGizmoModeKeys(snap);
        UpdateEditorCamera(snap, inViewport, lx, ly);
        UpdateGizmoInteraction(snap, inViewport, lx, ly, fs.gameUiClaims);
    }

    // The one "may editor shortcuts fire" predicate (architecture pass sec 1):
    // collapses the three near-identical gates the phase methods below used to
    // hand-roll (undo/redo+scene shortcuts, gizmo mode keys, camera framing).
    bool EditorApp::ShortcutsLive(const Arcane::InputSnapshot& snap,
                                  bool requireViewportFocus) const
    {
        return !InPlayMode() && !snap.wantCaptureKeyboard
            && (!requireViewportFocus || m_viewportActive);
    }

    // Phase 6a: undo/redo + the Ctrl+N/O/S scene shortcuts. Runs EARLIER in the
    // frame than the gizmo phase below, which is why the open-transaction guard
    // is here rather than there.
    void EditorApp::HandleUndoRedoAndSceneShortcuts(const Arcane::InputSnapshot& snap, FrameState& fs)
    {
        // Undo/redo keybinds: Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo.
        // Edge-triggered off the raw hardware snapshot (InputSnapshot has
        // no built-in press-edge tracking -- InputActions.Pressed() layers
        // that on named/JSON-configured actions, which would need a
        // separate "undo"/"redo" chord binding; hand-rolled here instead
        // to avoid a same-frame double-fire: an InputActions chord match
        // is a pure AND of its keys, so a bare "ctrl+z" binding would also
        // match while Shift is held, firing Undo alongside Redo).
        // snap.wantCaptureKeyboard is already baked from
        // m_gpu->Imgui().WantCaptureKeyboard() above, so this naturally
        // suppresses the keybind while typing in an ImGui text field --
        // the same suppression point InputActions::ResolveControl uses.
        // Edit-mode only: Play routes all input to the plugin.
        const bool ctrl  = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);
        const bool shift = snap.ScancodeDown(kScLShift) || snap.ScancodeDown(kScRShift);
        const bool undoKeyDown = ctrl && !shift && snap.ScancodeDown(kScZ);
        const bool redoKeyDown = ctrl && ((shift && snap.ScancodeDown(kScZ)) || (!shift && snap.ScancodeDown(kScY)));
        m_edges.undo.Update(undoKeyDown);
        m_edges.redo.Update(redoKeyDown);

        const bool active = ShortcutsLive(snap, false);
        // Also refuse while a transaction is open (e.g. a live gizmo drag):
        // CommandStack::Undo()/Redo() have no open-transaction guard, and this
        // keybind block runs earlier in the frame than the gizmo block below,
        // so an Undo here would apply the previous entry while the drag's own
        // "Gizmo" transaction is still holding pre-undo `before` bytes -- its
        // later Commit then pushes a transaction whose `before` predates the
        // undo and clobbers the redo entry. Ctrl is also the gizmo SNAP
        // modifier, so Ctrl-held drags are the normal case, not an edge case.
        const bool noOpenTxn = !m_undo->InTransaction();
        if (active && noOpenTxn && m_edges.undo.pressed) m_undo->Undo();
        if (active && noOpenTxn && m_edges.redo.pressed) m_undo->Redo();

        // Ctrl+N / Ctrl+O / Ctrl+S -- the shortcuts the File menu prints
        // beside New Scene / Open Scene / Save Scene. Raised as requests
        // rather than acted on here so both routes share ONE handler: the
        // menu-request site (below, after BeginDockSpace) owns the
        // unsaved-changes guard and the dialog launches.
        // It owns NO Play gate. These edges are Play-gated by `active`
        // above and the menu items by their own !playing enable flag
        // inside BeginDockSpace (EditorPanels.cpp) -- but neither
        // covers the routes that reach a save without passing through
        // them (the confirm modal's Save button, the Save As dialog
        // result). The gate that holds for all of them is DoSaveScene's
        // own refusal while playing.
        const bool nDown = ctrl && !shift && snap.ScancodeDown(kScN);
        const bool oDown = ctrl && !shift && snap.ScancodeDown(kScO);
        const bool sDown = ctrl && !shift && snap.ScancodeDown(kScS);
        m_edges.n.Update(nDown);
        m_edges.o.Update(oDown);
        m_edges.s.Update(sDown);
        fs.scNewScene  = active && m_edges.n.pressed;
        fs.scOpenScene = active && m_edges.o.pressed;
        fs.scSaveScene = active && m_edges.s.pressed;

        // Ctrl+X/C/V/D -- the Edit menu's clipboard items. Same raised-as-
        // request shape as Ctrl+N/O/S above: ONE handler at the menu-request
        // site. `active` already suppresses these while ImGui captures the
        // keyboard (text fields keep their own clipboard) and during Play.
        const bool xDown = ctrl && !shift && snap.ScancodeDown(kScX);
        const bool cDown = ctrl && !shift && snap.ScancodeDown(kScC);
        const bool vDown = ctrl && !shift && snap.ScancodeDown(kScV);
        const bool dDown = ctrl && !shift && snap.ScancodeDown(kScD);
        m_edges.x.Update(xDown);
        m_edges.c.Update(cDown);
        m_edges.v.Update(vDown);
        m_edges.d.Update(dDown);
        fs.scCut       = active && m_edges.x.pressed;
        fs.scCopy      = active && m_edges.c.pressed;
        fs.scPaste     = active && m_edges.v.pressed;
        fs.scDuplicate = active && m_edges.d.pressed;
    }

    // Phase 6b: gizmo mode keys. Pure state, but it runs before the gizmo
    // interaction phase so a mode change takes effect on the same frame.
    void EditorApp::HandleGizmoModeKeys(const Arcane::InputSnapshot& snap)
    {
        // Gizmo mode keys: W=Translate, E=Rotate, R=Scale (SDL_SCANCODE_W/E/R --
        // no conflict with the Sandbox plugin's camera, which uses RMB-pan +
        // wheel-zoom only). Edge-triggered off the raw hardware snapshot, same
        // pattern as the undo/redo keybinds above. Edit-mode + viewport-focus
        // only, so typing/clicking in another panel never changes the gizmo mode.
        const bool wDown = snap.ScancodeDown(kScW);
        const bool eDown = snap.ScancodeDown(kScE);
        const bool rDown = snap.ScancodeDown(kScR);
        const bool qDown = snap.ScancodeDown(kScQ);
        m_edges.w.Update(wDown);
        m_edges.e.Update(eDown);
        m_edges.r.Update(rDown);
        m_edges.q.Update(qDown);
        const bool keysActive = ShortcutsLive(snap, true);
        // Q = Select (no gizmo); W/E/R activate a transform gizmo (UE5 tools).
        if (keysActive && m_edges.q.pressed)
        {
            m_gizmoEnabled = false;
        }
        if (keysActive && m_edges.w.pressed)
        {
            m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Translate;
        }
        if (keysActive && m_edges.e.pressed)
        {
            m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Rotate;
        }
        if (keysActive && m_edges.r.pressed)
        {
            m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Scale;
        }
    }

    // Phase 6c: editor viewport camera. Its tail holds the FIRST of the frame's
    // two SetCamera pushes -- see the comment there; the gizmo phase below reads
    // what it writes.
    void EditorApp::UpdateEditorCamera(const Arcane::InputSnapshot& snap, bool inViewport, float lx, float ly)
    {
        // Editor viewport camera (Edit mode): RMB-drag pans, wheel zooms
        // at the cursor, F frames the selection (everything when nothing
        // is selected), Home frames everything. The plugin no longer sees
        // RMB/wheel in Edit mode (see the pluginSnap mask above), so the
        // two cameras cannot fight over the same gesture.
        const bool      rmbDown = (snap.mouseButtons & 0x2u) != 0;
        m_edges.rmb.Update(rmbDown);
        const glm::vec2 mouseWindow(snap.mouseX, snap.mouseY);
        if (!InPlayMode())
        {
            // A pan may only START over the viewport, but once
            // started it keeps tracking anywhere -- same rule as the
            // gizmo drag, so crossing the panel edge mid-drag does
            // not strand the view.
            if (m_edges.rmb.pressed && inViewport)
            {
                m_camPan.panning = true;
            }
            if (!rmbDown)
            {
                m_camPan.panning = false;
            }
            // RMB held across BOTH frames, so m_camPan.lastMouse is a
            // real previous cursor and the press edge cannot jump the
            // view by the whole distance from wherever the cursor last
            // was (same guard as Sandbox's Interaction pan). Equivalence:
            // m_camPan.panning && m_edges.rmb.down && !m_edges.rmb.pressed has
            // the same truth table as the old m_camPanning && m_prevRmbDown
            // -- m_camPan.panning implies rmbDown this frame (the `!rmbDown`
            // branch above clears it otherwise), so `down` is redundant and
            // `!pressed` (not a fresh rising edge) is exactly "was also down
            // last frame".
            if (m_camPan.panning && m_edges.rmb.down && !m_edges.rmb.pressed)
            {
                m_camera.Pan(mouseWindow - m_camPan.lastMouse);
            }

            // Zoom anchors on the viewport-local cursor, the space
            // the camera offset itself lives in. Deliberately NOT
            // gated on snap.wantCaptureMouse: it is true over the
            // viewport image by design (see the pluginSnap comment
            // above); inViewport already folds in m_viewportActive,
            // which is false whenever another panel owns the cursor.
            if (inViewport && snap.wheelY != 0.0f)
            {
                m_camera.ZoomAt(glm::vec2(lx, ly), snap.wheelY);
            }
        }
        else
        {
            m_camPan.panning = false;   // Play owns the pointer; drop any live pan
        }
        m_camPan.lastMouse = mouseWindow;

        // F / Home framing. Gated on wantCaptureKeyboard exactly like
        // the Ctrl+N/O/S shortcuts above, so F does not fire while a
        // text field or the Outliner rename box has focus. Unlike the
        // W/E/R gizmo keys this does NOT require viewport focus:
        // those switch a viewport TOOL, while framing acts on the
        // selection, and picking an entity in the Outliner and
        // pressing F is the point of the shortcut.
        const bool fDown    = snap.ScancodeDown(kScF);
        const bool homeDown = snap.ScancodeDown(kScHome);
        m_edges.f.Update(fDown);
        m_edges.home.Update(homeDown);
        const bool framingActive = ShortcutsLive(snap, false);
        if (framingActive && m_edges.f.pressed)
            FrameCamera(m_selection.HasSelection());
        if (framingActive && m_edges.home.pressed)
            FrameCamera(false);

        // Push the editor camera BEFORE the gizmo block below reads
        // Runtime::CameraOffset/CameraZoom. Those reads happen earlier
        // in the frame than the pre-SubmitRender push further down, so
        // without this the gizmo would hit-test against whatever camera
        // was stored LAST frame -- and on the very first frame against
        // the identity (zoom 1), placing the handles 100x off the
        // sprite. The click-pick and gizmo DRAW sites run after the
        // later push and are already consistent with it.
        if (!InPlayMode())
            m_runtime->SetCamera(m_camera.offset, m_camera.zoom);
    }

    // Phase 6d: transform-gizmo hit-test + drag. Reads the camera the phase
    // above just pushed; must stay after it.
    void EditorApp::UpdateGizmoInteraction(const Arcane::InputSnapshot& snap, bool inViewport,
                                           float lx, float ly, bool gameUiClaims)
    {
        // Transform-gizmo interaction: hit-test + drag against the selected
        // entity's Transform, bracketed into the undo stack (one drag =
        // one undo step -- Begin/SnapshotComponent on press, Commit on release;
        // a no-move drag self-drops since Commit only pushes if bytes changed).
        // mouseScreen is viewport-local px (lx/ly computed above), the same
        // space CameraOffset/CameraZoom register in, so the gizmo aligns
        // pixel-for-pixel with the scene (mirrors the click-pick's PickView
        // below). LMB edges are tracked unconditionally each frame (like the
        // mode keys above) so a button already held before the cursor enters
        // the viewport is never misread as a fresh press. Deliberately does
        // NOT gate on snap.wantCaptureMouse -- it is true over the viewport
        // image by design (see the pluginSnap comment above); `inViewport`
        // already folds in m_viewportActive.
        const bool lmbDown = (snap.mouseButtons & 0x1u) != 0;
        m_edges.lmb.Update(lmbDown);
        const bool mousePressedLeft  = m_edges.lmb.pressed;
        const bool mouseReleasedLeft = m_edges.lmb.released;
        const glm::vec2 mouseScreen(lx, ly);
        const bool ctrlHeld = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);

        // An in-progress drag must keep tracking (and commit on release)
        // even after the cursor leaves the viewport rect -- only a FRESH
        // drag requires the cursor in-viewport. Aborting a live drag on
        // viewport-exit would strand the Transform at a mid-drag value
        // with no undo record (CommandStack::Cancel does not revert), so
        // viewport-exit is deliberately NOT an abort. !gameUiClaims is
        // defensive: gameUiClaims is Play-only and this is already
        // !IsPlaying()-gated, so it is a no-op today, but it keeps this
        // gate correct if the gizmo is ever allowed to run in Play.
        //
        // GizmoLive() rather than m_gizmoEnabled (NRI Phase 3, Task 13
        // follow-up): a golden run holds a SCRIPTED selection now, so this gate
        // is one W keypress away from arming a live hit-test against the desk
        // mouse -- and a stray press on a handle would MOVE the entity
        // mid-capture. See the predicate's own comment.
        const bool gizmoActive = !InPlayMode() && !gameUiClaims &&
                                 GizmoLive() && m_selection.HasSelection() &&
                                 (m_gizmoDrag.active || inViewport);
        Astra::Registry*        regPtr = nullptr;
        Arcane::Transform* lt     = nullptr;
        if (gizmoActive)
        {
            regPtr = &m_runtime->Registry();
            lt = regPtr->GetComponent<Arcane::Transform>(m_selection.Primary());
        }

        if (lt)
        {
            const Astra::Entity sel = m_selection.Primary();
            // WORLD pose, not local -- Transform is parent-local, so
            // anchoring/hit-testing the gizmo at the local values would
            // misplace it for any parented entity (Unreal parity: the
            // gizmo pivot is the primary's world location; see the
            // group-delta conversion below for the write-back half).
            const Arcane::GizmoTransform gt =
                Arcane::DecomposeTRS(Arcane::Edit::WorldMatrix(*regPtr, sel));
            const Arcane::GizmoView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };

            if (!m_gizmoDrag.active)
            {
                // Hover + drag-start only when the cursor is over the viewport.
                if (inViewport)
                {
                    m_gizmoHovered = Arcane::HitTest(m_gizmoMode, m_gizmoSpace, gt, view, mouseScreen);
                    if (m_gizmoHovered != Arcane::GizmoAxis::None && mousePressedLeft)
                    {
                        // A press on a handle owns the click regardless of
                        // whether the descriptor resolves, so it never falls
                        // through to the click-pick below.
                        m_gizmoCapturedClick = true;
                        const Astra::ComponentDescriptor* desc =
                            FindTransformDescriptor(*regPtr, sel);
                        if (desc)
                        {
                            // Roots only -- see GizmoDrag::targets. One
                            // Begin + N SnapshotComponent = ONE undo step:
                            // CommandStack dedupes per (entity, descriptor)
                            // and Commit packs them all into one transaction.
                            const std::vector<Astra::Entity> roots =
                                Arcane::Edit::SelectionRoots(*regPtr, m_selection.Entities());
                            m_gizmoDrag.targets.clear();
                            m_gizmoDrag.targets.reserve(roots.size());
                            // Keep the token: only its holder may close
                            // this transaction (see GizmoDrag::txn).
                            m_gizmoDrag.txn = m_undo->Begin("Gizmo");
                            for (Astra::Entity e : roots)
                            {
                                Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                                if (!et)
                                    continue;   // non-spatial node in the selection
                                const Astra::ComponentDescriptor* ed =
                                    FindTransformDescriptor(*regPtr, e);
                                if (!ed)
                                    continue;
                                m_undo->SnapshotComponent(e, ed);
                                // Stored WORLD pose (see gt above) -- the group
                                // delta below composes/replays in world space.
                                m_gizmoDrag.targets.push_back(
                                    { e, Arcane::DecomposeTRS(Arcane::Edit::WorldMatrix(*regPtr, e)) });
                            }
                            m_gizmoDrag.active           = true;
                            m_gizmoDrag.axis             = m_gizmoHovered;
                            m_gizmoDrag.start            = gt;
                            m_gizmoDrag.mouseStartScreen = mouseScreen;
                        }
                    }
                }
                else
                {
                    m_gizmoHovered = Arcane::GizmoAxis::None;
                }
            }
            else
            {
                // Current cursor in viewport-local px, EXTRAPOLATED past the
                // rect edges (ToViewportLocal writes lx/ly even when it returns
                // false) so the drag keeps tracking while the cursor is outside
                // the viewport. Recomputed here because the shared lx/ly are
                // only written when m_viewportActive is set.
                float dragLx = 0.0f, dragLy = 0.0f;
                Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, dragLx, dragLy);
                const glm::vec2 dragMouse(dragLx, dragLy);

                Arcane::GizmoSnap gsnap;
                gsnap.enabled = ctrlHeld;
                const Arcane::GizmoTransform nt = Arcane::ApplyDrag(
                    m_gizmoMode, m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view,
                    m_gizmoDrag.mouseStartScreen, dragMouse, gsnap);
                // One delta from the primary's drag, replayed onto every
                // target's PRE-drag pose -- recomputed from `start` each
                // frame, so nothing accumulates drift. The primary is in
                // `targets` when it is itself a root and round-trips
                // exactly (see ApplyGroupDelta).
                const Arcane::GizmoGroupDelta gd =
                    Arcane::MakeGroupDelta(m_gizmoDrag.start, nt);
                for (const auto& [e, startPose] : m_gizmoDrag.targets)
                {
                    Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                    if (!et)
                        continue;   // destroyed mid-drag
                    // startPose/gd are WORLD; convert the new world pose back
                    // through the parent's inverse before writing the LOCAL
                    // Transform (Unreal's SetWorldTransform demotes to relative
                    // when attached -- this is that demotion).
                    const Arcane::GizmoTransform w = Arcane::ApplyGroupDelta(startPose, gd);
                    const glm::mat3 localMat =
                        glm::inverse(Arcane::Edit::ParentWorldMatrix(*regPtr, e)) * Arcane::ComposeTRS(w);
                    const Arcane::GizmoTransform r = Arcane::DecomposeTRS(localMat);
                    et->position = r.position;
                    et->rotation = r.rotation;
                    et->scale    = r.scale;
                }
                m_gizmoHovered = m_gizmoDrag.axis;   // keep the active handle highlighted

                if (mouseReleasedLeft)
                {
                    m_undo->Commit(m_gizmoDrag.txn);   // no-move drag self-drops (after == before)
                    m_gizmoDrag          = {};         // clears txn back to None
                    m_gizmoCapturedClick = true;
                }
            }
        }
        else
        {
            m_gizmoHovered = Arcane::GizmoAxis::None;
            // Only reached with an active drag when the entity/component
            // genuinely vanished (deleted/deselected mid-drag) -- discard the
            // now-meaningless transaction.
            if (m_gizmoDrag.active)
            {
                m_undo->Cancel(m_gizmoDrag.txn);
                m_gizmoDrag = {};
            }
        }
    }

    // Phase 7: sim advance. The plugin's UpdateAll runs HERE, before the
    // pre-submit camera push below -- that ordering is what lets the editor
    // camera override a plugin that pushes its own every frame (Sandbox does).
    void EditorApp::AdvanceSim(LoopState& ls)
    {
        // Sim advance through the RunLoop with the plugin callbacks interleaved.
        const auto now = std::chrono::steady_clock::now();
        double simDt = std::chrono::duration<double>(now - ls.simPrev).count();
        ls.simPrev = now;
        if (simDt > 0.25) simDt = 0.25;
        // NRI Phase 3, Task 13: pinned under golden mode -- see FrameInput's
        // frameDt pin just above this phase for the shared reasoning, and
        // RuntimeFrame::AdvanceSim's matching pin (RuntimeFrame.cpp:253) for
        // the runtime's half of the same rule. One fixed 60 Hz step per
        // rendered frame, so --frames N gives N identical steps regardless of
        // how fast this desk is running.
        if (m_config.GoldenMode())
            simDt = 1.0 / 60.0;
        m_runtime->Loop().Advance(simDt,
            [&](double dt)          { if (m_plugin) m_plugin->FixedUpdateAll(dt); },
            [&](double dt, double a){ if (m_plugin) m_plugin->UpdateAll(dt, a); });
        m_runtime->AudioSystem().Update(simDt);
    }

    // Phase 8: deferred viewport resize. Must stay before the scene render.
    void EditorApp::ApplyPendingViewportResize()
    {
        m_viewportTargets.ApplyPendingResize(m_graphChrome.get());
    }

    // Deferred resize: the Viewport panel's content-region size measured LAST
    // frame, applied at the START of THIS frame (before this frame records).
    // This mirrors the m_viewportRect/m_viewportActive one-frame lag on
    // EditorApp -- it avoids a same-frame use-after-free where a resize
    // (called right after ImGui::Image bakes the current texture pointer into
    // this frame's draw list) synchronously frees that very texture before
    // ImGui replays the draw list at Render time: ResizeOffscreen destroys the
    // output texture synchronously. (NRI Phase 5a, Task 4: this used to also
    // guard the NVRHI arm's OffscreenCanvas::Resize for the identical reason;
    // that arm is deleted.)
    void EditorApp::ViewportTargets::ApplyPendingResize(Arcane::NriGraphContext* chrome)
    {
        // ================= THE GRAPH ARM (NRI Phase 3, Task 8) =============
        // THE CODIFIED RESIZE CONTRACT. Its shape is prescribed, clause for
        // clause, on NriGraphContext::ResizeOffscreen's declaration (and item
        // (2) of that header's TWO CONTEXTS, TWO LANES block). Read it before
        // touching these five lines; what follows is why each one is where it
        // is, not a restatement.
        //
        //   (i) THE SIZE GUARD IS OURS. ResizeOffscreen no-ops on an unchanged
        //       size, but that guard is INSIDE the call -- after
        //       InvalidateUserTextureNow has already idled the whole device
        //       unconditionally. Without the test below, a panel that is
        //       simply sitting there would stall the device EVERY FRAME,
        //       forever, on a size that never changed. SurfaceWidth/Height are
        //       exactly what ResizeOffscreen compares against, so the two tests
        //       cannot disagree.
        //
        //  (ii) INVALIDATE **BEFORE**, WITH THE `Now` VARIANT. The chrome
        //       context's ImGuiNri caches by RAW nri::Texture*, and NRI does
        //       not ref-count -- the replacement may land on the freed address,
        //       so a pointer compare cannot detect the swap and re-reading
        //       OffscreenTextureId() is not a substitute. Invalidating AFTER
        //       (or with the deferred variant) would bury the CHROME backend's
        //       view over an ALREADY-DESTROYED texture into the CHROME lane --
        //       DestroyDescriptor after DestroyTexture, every resize, for the
        //       one descriptor that inherently spans both contexts, which no
        //       lane ordering can repair because nothing orders two graveyards.
        //       `Now` destroys the view inside the call behind its own idle, so
        //       run first it provably dies while the texture is still alive.
        //       Unconditional, never gated on the pointer having changed: an
        //       unchanged pointer is precisely the case a recycled address
        //       fakes.
        //
        // (iii) THE PAIR IS ONE OPERATION. Nothing may render between them --
        //       after the invalidate there is NO cache entry for `old`, so a
        //       chrome frame recorded in between would take EnsureEntry's
        //       CREATE path and build a fresh view + descriptor set over a
        //       texture about to be destroyed. Hence: same statement block, no
        //       frame boundary, no early-out, nothing added between these two
        //       lines. `chrome` is guarded like every in-tree ImGuiHud() caller
        //       (it is null only if the chrome context failed to create, which
        //       CreateGraphVehicles refuses to continue past).
        //
        // COST, stated because it is visible: two full device idles per
        // resizing frame during a viewport drag-storm. That is the accepted
        // price of the ordering; a stall that OUTLIVES the drag would mean (i)
        // is broken. See the DESK WATCH block on ResizeOffscreen.
        // NRI Phase 5a, Task 4: the NVRHI arm this used to fall through to
        // below (a null `graph` used to mean "an NVRHI viewport instead" --
        // resizing `canvas`/`pick`/`outline` in lockstep) is deleted along
        // with those three members. A null `graph` here now means only what
        // the graph-session comment already named: a project switch whose
        // rebuild failed (SwitchProject's "render_bridge", NRI Phase 3 Task
        // 12), already exiting -- this frame still has to finish, and it
        // finishes by doing nothing here.
        if (!graph)
            return;

        if (pendingW != 0 && pendingH != 0 &&
            (pendingW != graph->SurfaceWidth() || pendingH != graph->SurfaceHeight()))
        {
            nri::Texture* const old = graph->OffscreenOutput();
            if (chrome && chrome->ImGuiHud())
                chrome->ImGuiHud()->InvalidateUserTextureNow(old);   // BEFORE
            graph->ResizeOffscreen(pendingW, pendingH);
        }
    }

    // Phase 9: scene asset resolution + the Edit-mode derived-transform refresh
    // + the SECOND camera push. All three sit immediately before the scene
    // submit in the next phase, and each says below why it has to.
    void EditorApp::RefreshSceneResolution()
    {
        // Sprite / sprite-material / post-chain resolution, in ONE engine-side
        // call the standalone runtime host makes too (sprite-resolution lift).
        // It sweeps the scene for referenced Guids, pumps the compile service
        // (routing results to open documents first, via the consumeFirst hook
        // installed in EditorApp::Init), and publishes the registry's
        // SpriteTable + SpriteMaterialTable.
        //
        // WHY HERE, before RenderSceneToViewport rather than after it: the
        // tables this publishes are what THIS frame's submission reads. The
        // pre-lift editor swept and published in phase 13, AFTER the scene
        // render, so every table update was one frame late -- a newly referenced
        // sprite drew the 1x1 placeholder for a frame first. It also must stay
        // outside the frame batcher's Begin..Drain, since the drain REGISTERS
        // materials with it.
        //
        // The compile clock advances here (phase 13 advanced it pre-lift) because
        // everything downstream -- Request debounce, material globals, document
        // Submits -- reads it, and it must advance exactly ONCE per frame.
        m_editorClock += m_gameUi.frameDt;

        if (m_resolver)
        {
            Arcane::SceneRenderResolver::FrameInfo frame;
            frame.now            = m_editorClock;
            frame.dt             = m_gameUi.frameDt;
            frame.viewportWidth  = (float)ViewportWidth();
            frame.viewportHeight = (float)ViewportHeight();
            m_resolver->Refresh(frame);
        }
        // Derived transforms, refreshed for THIS frame before anything reads
        // them.
        //
        // TransformPropagationSystem is a fixedUpdate system, and Edit mode
        // holds the RunLoop paused -- so the whole fixed phase is frozen while
        // SubmitRender still runs every frame. Without this, WorldTransform is
        // never computed (nor materialised) in Edit mode: sprites do not draw
        // at all until you press Play, and a gizmo drag moves Transform with
        // nothing on screen following it. Play mode does not need this, since
        // the fixed phase is running and would do the same work twice.
        //
        // World transforms are DERIVED data: whoever reads them is responsible
        // for them being current, and in Edit mode that is the editor.
        if (!InPlayMode())
        {
            Arcane::TransformPropagationSystem{}(m_runtime->Registry());
            // NRI Phase 3, Task 13 follow-up: THE GOLDEN VIEW/SELECTION PIN,
            // standing exactly where the ordinary run's deferred first-open
            // framing stands. Same discipline as the clock pin in FrameInput/
            // AdvanceSim -- `GoldenMode() ? pinned : live` -- applied to the
            // other two inputs that decide what the captured picture CONTAINS
            // rather than merely when it is taken. HERE and not in phase 6c
            // (UpdateEditorCamera) for two reasons: this is after
            // ApplyPendingViewportResize, so the fit sees THIS frame's viewport
            // extent, and it is before the SetCamera push below -- the one
            // every render path on both arms reads. See PinGoldenViewport.
            if (m_config.GoldenMode())
                PinGoldenViewport();
            else
                FrameSceneIfPending();
        }

        // Editor camera -> the Runtime slot SetRenderContext reads, for
        // the SECOND time this frame (the first is in the input block, so
        // the gizmo hit-test sees it). It has to be re-pushed HERE, after
        // the plugin's UpdateAll ran above: a plugin that pushes its own
        // camera every frame -- Sandbox does -- would otherwise own the
        // Edit-mode view. Must stay after that Advance and before
        // SubmitRender below; moving it earlier hands the view back to
        // the plugin.
        //
        // Play mode pushes the SCENE's camera instead of the editor's, so
        // play-in-editor shows what a standalone ArcaneRuntime would show --
        // both now resolve the view from the same place (ActiveSceneCamera).
        // Falls through to the plugin's own camera when the scene has no
        // active Camera entity, which is what keeps the Sandbox showcase
        // (a plugin that drives its own camera) working unchanged.
        if (!InPlayMode())
        {
            m_runtime->SetCamera(m_camera.offset, m_camera.zoom);
        }
        else if (const auto sceneCam = Arcane::ActiveSceneCamera(
                     m_runtime->Registry(),
                     (float)ViewportWidth(), (float)ViewportHeight()))
        {
            m_runtime->SetCamera(sceneCam->offset, sceneCam->zoom);
        }

        // NRI Phase 5a, Task 4: THE POST CHAIN'S HANDOFF used to fork here --
        // the NVRHI arm installed the built FullscreenMaterialChain on the
        // canvas (SetPostChain/SetPostGlobals, reading the postChain/postInst
        // /postGlobals locals this function used to compute above), reading
        // SceneRenderResolver::PostChain()/PostInstance(). Both that chain and
        // its only caller are deleted. The graph arm's half of the handoff was
        // always the resolver running device-less (StageSpriteTables) and
        // PostChainCache publishing BYTES instead -- those travel to the
        // recorder as FrameDesc::post + FrameDesc::globals, read fresh at the
        // render site in the next phase (RenderSceneToViewport). That is the
        // only handoff left; there is nothing further to do here.
    }

    // Phase 10: the scene render. Must run BEFORE the ImGui pass builds the
    // Viewport panel's Image, and the gizmo inside it is drawn AFTER the scene
    // submit so it renders on top.
    void EditorApp::RenderSceneToViewport()
    {
        // ================= THE GRAPH ARM (NRI Phase 3, Task 8) =============
        // The SAME submission, drained by a different recorder. Built to match
        // RuntimeFrame::RenderGraph field for field, because the D3c goldens
        // compare these two frames: the null command list / null framebuffer in
        // Begin are what say "record nothing" (Batcher2D::Begin only stores
        // them, and every recording use is inside End(), which this path never
        // calls -- and could not, the batcher being device-less since
        // GpuContext::Create built it that way).
        //
        // AS OF TASK 9 THIS PHASE DECLARES THE WHOLE VIEWPORT FRAME -- the
        // scene, the game HUD (phase 11's content) and the pick + outline chain
        // (phases 12 and 17's), because on this recorder all three are NODES in
        // ONE declaration rather than three passes submitted at three points in
        // the frame. ArmGraphViewportFrame below is the other phases' half; see
        // its comment for why it has to run HERE and not where those phases sit.
        //
        // WHAT THIS FRAME CARRIES, so the gap stays named rather than
        // discovered:
        //   * imgui        -- HOST CHROME's draw data. Not the game HUD (that
        //                     is `gameUi`): this viewport has no chrome on it,
        //                     and the editor's own chrome is Task 10's, on the
        //                     CHROME context. STILL absent -- unaffected by
        //                     Task 13.
        //   * capture      -- the auto-screenshot read (Task 11) still runs
        //                     through CaptureGraphViewportPng's OWN fresh
        //                     frame, not this one. The GOLDEN capture (Task
        //                     13) piggybacks on THIS declaration instead, on
        //                     the run's last frame only -- see below.
        //   * stage        -- Task 13: the editor's own golden-stage
        //                     vocabulary, unconditional exactly like
        //                     RuntimeFrame::RenderGraph's graphFrame.stage.
        //                     Full outside golden mode (HostConfig::Parse
        //                     refuses a non-Full stage otherwise), so this is
        //                     a no-op on every ordinary run.
        //                     DeclareGraphFrame's own stage gate (`batch`
        //                     drops the post chain) is what actually bypasses
        //                     vp.post below -- no separate check is needed
        //                     here for it, unlike the NVRHI arm's
        //                     RefreshSceneResolution, where the canvas' post
        //                     binding is set-then-forget rather than re-read
        //                     per frame.
        if (m_viewportTargets.graph)
        {
            const std::uint32_t w = ViewportWidth();
            const std::uint32_t h = ViewportHeight();

            Arcane::Batcher2D& b = m_gpu->Batch();
            b.Begin(w, h);
            SubmitSceneToBatcher(b);

            // Copied, not borrowed, by FrameDesc -- but the local has to
            // outlive the call all the same, and it is the SAME GlobalParams
            // the batcher just got via SubmitSceneToBatcher's SetGlobals, so
            // the two recorders cannot bind different b1 contents.
            const Arcane::GlobalParams globals =
                m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{};

            Arcane::NriGraphContext::FrameDesc vp;
            vp.batch   = &b;
            // Re-read every frame, like the runtime's: a drain may swap the
            // bound instance under an asset re-save. Null (no PostProcess
            // assignment, or the compile has not landed) simply renders
            // canvas -> tonemap. `batch`-stage bypass happens inside
            // DeclareGraphFrame (shape.stage != GoldenStage::Batch) -- see the
            // header comment above.
            vp.post    = m_resolver ? m_resolver->PostDesc() : nullptr;
            vp.globals = &globals;
            vp.stage   = m_config.goldenStage;
            // Phases 11, 12 and 17's contribution to THIS declaration. Strictly
            // AFTER SubmitSceneToBatcher above, which is not incidental: it runs
            // the plugin's DrawUI, and on the NVRHI arm that call also lands
            // after the scene submission (phase 11 follows phase 10). A DrawUI
            // that mutates the scene must therefore land on the same frame on
            // both arms.
            ArmGraphViewportFrame(vp);

            // NRI Phase 3, Task 13: the golden capture, armed on the LAST
            // frame of a golden run only -- the same "+1" the runtime's
            // RenderGraph uses (io.frameCount is bumped in CaptureTail, i.e.
            // after this arm returns; here EndFrame's own m_frameCount
            // increment happens later still, at phase 20). Piggybacking on
            // the frame ALREADY being declared here, rather than a second
            // render the way CaptureGraphViewportPng does for the
            // auto-screenshot, is what keeps the captured picture governed by
            // the SAME stage gate as every other frame of the run -- a
            // separate render call would have to re-derive vp.stage and
            // ArmGraphViewportFrame's suppression rather than inherit them.
            // WIDENED TO --screenshot (whole-branch review, I4): both flags are
            // shared HostConfig members that this host PARSES, and until now
            // only the golden pair armed a capture -- so `ArcaneEditor
            // --screenshot out.png --frames 60` exited 0 having written
            // nothing. The runtime's CaptureTail has always armed on
            // `!screenshotPath.empty() || GoldenMode()`; this is that predicate,
            // on this host. `maxFrames != 0` stays, and matches the runtime:
            // both capture on the LAST frame, and without --frames there is no
            // last frame.
            const bool isCaptureLastFrame = m_config.maxFrames != 0 &&
                                            (m_frameCount + 1) >= m_config.maxFrames &&
                                            (m_config.GoldenMode() || !m_config.screenshotPath.empty());
            vp.capture = isCaptureLastFrame;

            const Arcane::NriGraphContext::FrameOutcome outcome =
                m_viewportTargets.graph->RenderFrameOffscreen(vp);
            // Skipped stays UNACTED-ON, and that is the routine case: a
            // collapsed panel, or a resize whose replacement could not be
            // created -- OffscreenTextureId() is 0 then and phase 16 draws no
            // Image, which IS the signal. A golden run whose last frame lands
            // Skipped simply does not capture this iteration -- m_frameCount
            // is driven by phase 19/20 (the CHROME frame's own success), not
            // by this outcome, so a persistently collapsed panel is a desk
            // condition D3c would notice long before a golden capture matters
            // (see the task-13 report's concerns).
            //
            // Failed is the half Task 10 closes (it was listed here as owed).
            // It has already bumped RenderErrorCount, so the run's exit code
            // would be 2 on its own -- but 2 says "a validation error fired"
            // where 1 says WHERE the run died, and a viewport frame that could
            // not be recorded or submitted is the second thing. NoteGraphFrame-
            // Failure records both halves: the exit code and the request to
            // stop.
            //
            // THE REST OF THIS FRAME STILL RUNS, and the reason is NOT an ImGui
            // pairing: nothing is owed at this point in the frame. Phase 10
            // runs BEFORE DrawEditorUi (phase 13) begins the editor's frame, so
            // there is nothing begun to pair; and the GAME context's frame is
            // begun and paired entirely inside ArmGraphViewportFrame above,
            // before the call that just failed. Bailing here would strand
            // neither.
            //
            // The reason is that this phase has no stop channel and should not
            // grow one: RenderSceneToViewport returns void, and threading an
            // immediate bail through the nine phases between here and the loop
            // bottom would be real control flow added for a case that costs one
            // more chrome frame. That frame is safe to run -- if the device is
            // genuinely gone it fails too and folds to the same exit code (1,
            // first-failure-wins). So the exit lands at the TOP of the next
            // frame instead, where PumpFrameEvents reads m_requestExit before
            // any phase runs: nothing renders after this frame.
            if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
            {
                NoteGraphFrameFailure("the viewport frame could not be recorded or submitted");
                return;
            }

            // The golden readback, RIGHT HERE rather than a later phase:
            // ReadCapture is synchronous (it idles the device internally --
            // the same stall CaptureGraphViewportPng pays, and this call is
            // rare -- ordinarily exactly once, on the run's last frame), and
            // the captured pixels belong to the frame that was JUST
            // presented -- reading them any later risks the next frame's
            // Begin/Draw overwriting the vehicle's transient state first.
            //
            // NOT ALWAYS EXACTLY ONCE, though (review fix round 1's
            // correction, mirrored from CaptureEditorGolden's own comment):
            // if the CHROME frame (phase 19, PresentFrame/PresentChromeFrame)
            // fails to present this same iteration, MainLoop `continue`s
            // WITHOUT running EndFrame, m_frameCount does not advance, and
            // `isGoldenLastFrame` is unchanged on the retry -- so a second
            // viewport frame that DOES present here re-attempts the capture
            // and simply overwrites the same file with equivalent content.
            // Harmless, and orthogonal to whatever made the chrome frame
            // fail. m_goldenCaptured is set on every attempt, retries
            // included -- each one is a genuine capture against a real,
            // just-presented target.
            if (isCaptureLastFrame && outcome == Arcane::NriGraphContext::FrameOutcome::Presented)
            {
                if (m_config.GoldenMode())
                    m_goldenCaptured = true;
                std::uint32_t cw = 0, ch = 0;
                std::vector<unsigned char> actual;
                if (!m_viewportTargets.graph->ReadCapture(cw, ch, actual))
                {
                    // --screenshot has always degraded to a WARN and never an
                    // exit code (SaveTexturePng's contract, mirrored by
                    // RuntimeFrame::CaptureTail); only a golden run fails on a
                    // readback. Folding the two arms into one predicate must
                    // not quietly change that.
                    if (m_config.GoldenMode())
                    {
                        ARC_ERROR("golden: viewport capture readback failed");
                        m_goldenExit = 3;
                    }
                    else
                    {
                        ARC_WARN("screenshot FAILED: {} (viewport capture readback)",
                                 m_config.screenshotPath);
                    }
                }
                else
                {
                    if (!m_config.screenshotPath.empty())
                    {
                        if (Arcane::WritePngRgba(m_config.screenshotPath, cw, ch, actual.data()))
                            ARC_INFO("screenshot written: {}", m_config.screenshotPath);
                        else
                            ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                    }
                    if (m_config.GoldenMode()
                        && Arcane::GoldenArtifact(m_config, cw, ch, actual,
                                                  Arcane::kEditorGoldenNamePrefix) != 0)
                    {
                        m_goldenExit = 3;
                    }
                }
            }
            return;
        }

        // NRI Phase 5a, Task 4: a session that lost its viewport context to a
        // failed post-switch rebuild used to fall through here into the NVRHI
        // arm's target (`m_viewportTargets.canvas`, guarded the same way
        // ApplyPendingResize's `graph` null check is -- NRI Phase 3, Task 12).
        // That arm is deleted along with `canvas`, so there is nothing left to
        // fall through to: the exit is already requested, and this frame just
        // ends here without a fault.
    }

    // The scene submission plus the two Edit-mode overlays, against whichever
    // batcher the caller hands in. A PURE EXTRACTION of the lambda that used to
    // sit inside RenderSceneToViewport's (NVRHI-arm, now deleted)
    // OffscreenCanvas::Draw call -- every statement kept its position relative
    // to every other. It existed so the NVRHI and graph recorders provably
    // drained the SAME content, which is now historical: the graph arm is the
    // only caller left, but the extraction stays, since re-inlining it would
    // serve no purpose and the ordering guarantee it documents is still real.
    void EditorApp::SubmitSceneToBatcher(Arcane::Batcher2D& b)
    {
        // Globals for registered sprite materials (Time/Delta/
        // Viewport); built-in pipelines ignore them. Taken from the
        // resolver, which built them from the same frame in phase 9 --
        // this used to be a second hand-rolled copy of the same four
        // fields, and two copies of one fact is how the editor and the
        // runtime drifted apart in the first place.
        b.SetGlobals(m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{});

        m_runtime->SetRenderContext(&b);
        m_runtime->Loop().SubmitRender();

        // Transform gizmo, drawn AFTER the scene submit so it renders on
        // top. Frame ordering guarantees the input block above already ran
        // this frame, so m_gizmoHovered/m_gizmoDrag and the live
        // Transform read here are current. Edit-mode + has-selection
        // only (mirrors the interaction gate; no viewport-hover requirement
        // here -- the gizmo should stay visible while e.g. the mouse is over
        // the Inspector, just not be interactable there).
        // Camera rect: what the scene camera will actually capture, drawn
        // in the EDIT view so framing is authorable without guessing (the
        // affordance Unity's camera gizmo provides). Edit mode only -- in
        // Play the view IS the camera, so a rect would just trace the
        // viewport border.
        //
        // Asked for at the camera's OWN aspect (the viewport's), then drawn
        // through the EDITOR's camera: ActiveSceneCamera reports the authored
        // worldCenter/halfHeight precisely so a caller can draw the camera
        // instead of looking through it.
        if (!InPlayMode())
        {
            const float vw = (float)ViewportWidth();
            const float vh = (float)ViewportHeight();
            if (const auto cam = Arcane::ActiveSceneCamera(m_runtime->Registry(), vw, vh))
            {
                const float aspect = (vh > 0.0f) ? (vw / vh) : 1.0f;
                const glm::vec2 halfWorld(cam->halfHeight * aspect, cam->halfHeight);
                // screen = world * zoom + offset, the one canonical mapping
                // (SceneResources.hpp) -- here with the EDITOR's view, which
                // the phase above already pushed for this frame.
                const glm::vec2 eo = m_runtime->CameraOffset();
                const float     ez = m_runtime->CameraZoom();
                const glm::vec2 c  = cam->worldCenter * ez + eo;
                const glm::vec2 h  = halfWorld * ez;
                const glm::vec2 tl(c.x - h.x, c.y - h.y);
                const glm::vec2 br(c.x + h.x, c.y + h.y);
                // A thin desaturated line stays legible over both bright and
                // dark scene content without competing with the selection
                // outline or the gizmo axes. (Dashed would read better still,
                // but the batcher has no dash primitive.)
                const glm::vec4 col(0.45f, 0.62f, 0.78f, 0.75f);
                b.Line(glm::vec2(tl.x, tl.y), glm::vec2(br.x, tl.y), 1.0f, col);
                b.Line(glm::vec2(br.x, tl.y), glm::vec2(br.x, br.y), 1.0f, col);
                b.Line(glm::vec2(br.x, br.y), glm::vec2(tl.x, br.y), 1.0f, col);
                b.Line(glm::vec2(tl.x, br.y), glm::vec2(tl.x, tl.y), 1.0f, col);
            }
        }

        // GizmoLive() rather than m_gizmoEnabled (NRI Phase 3, Task 13
        // follow-up): this draw goes into the SCENE BATCH, so under a scripted
        // golden selection the gizmo would land in `batch` and `post` too --
        // and the scripted selection is only allowed to change `full`. There is
        // no stage gate that could confine it here; the batcher content IS the
        // batch stage. Pinned off in golden mode instead. Ordinary runs are
        // unchanged (GizmoLive() == m_gizmoEnabled there).
        if (!InPlayMode() && GizmoLive() && m_selection.HasSelection())
        {
            Astra::Registry& drawReg = m_runtime->Registry();
            Arcane::Transform* lt = drawReg.GetComponent<Arcane::Transform>(
                m_selection.Primary());
            if (lt)
            {
                // WORLD pose, matching the interaction block's gt above --
                // draws at the same place it hit-tests, including for a
                // parented primary.
                const Arcane::GizmoTransform gt = Arcane::DecomposeTRS(
                    Arcane::Edit::WorldMatrix(drawReg, m_selection.Primary()));
                const Arcane::GizmoView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                Arcane::Draw(b, m_gizmoMode, m_gizmoSpace, gt, view, m_gizmoHovered,
                            m_gizmoDrag.active ? m_gizmoDrag.axis : Arcane::GizmoAxis::None);
            }
        }
    }

    // Phase 11's CPU HALF (NRI Phase 3, Task 9) -- see the declaration. Every
    // statement here was inside CompositeGameUi below and kept its position
    // relative to every other; what moved out is only the GPU composite,
    // which is the one thing the NVRHI and graph arms genuinely did
    // differently (an NVRHI blit into the canvas' output framebuffer, versus
    // a node inside the graph frame) -- back when both arms existed. NRI
    // Phase 5a, Task 4 deleted the NVRHI arm's composite (CompositeGameUi is
    // now a standing no-op), so ArmGraphViewportFrame is this function's only
    // caller left.
    bool EditorApp::BeginGameUiFrame()
    {
        // Edit mode: not run (clean viewport); the input is reset so a stray
        // draw sees no cursor/buttons. THIS RESET IS NOT CONDITIONAL ON THE
        // ARM and must not become so -- it is what guarantees a game context
        // that is never begun in Edit still reads the cursor as off-target if
        // anything ever does begin it.
        if (!InPlayMode())
            m_gameImgui->SetInput({});

        const Arcane::PluginVTable* vtGame = m_plugin ? m_plugin->Vtable() : nullptr;
        if (!InPlayMode() || !vtGame || !vtGame->DrawUI)
            return false;

        Arcane::OffscreenImGuiLayer::Input gi;
        gi.displaySize  = glm::vec2((float)ViewportWidth(), (float)ViewportHeight());
        gi.deltaTime    = (float)m_gameUi.frameDt;
        gi.hasInput     = m_gameUi.inViewport;
        gi.mousePos     = m_gameUi.viewportMouse;
        gi.mouseDown[0] = (m_gameUi.mouseButtons & 0x1u) != 0;   // LMB
        gi.mouseDown[1] = (m_gameUi.mouseButtons & 0x2u) != 0;   // RMB
        gi.mouseDown[2] = (m_gameUi.mouseButtons & 0x4u) != 0;   // MMB
        gi.wheel        = m_gameUi.wheel;
        m_gameImgui->SetInput(gi);
        m_gameImgui->BeginFrame();
        m_plugin->DrawUIAll();   // game module + any secondary plugins, into the game context
        // The caller now owes EXACTLY ONE of Render / RenderToDrawData /
        // EndFrameDiscard. Returning true rather than doing it here is what
        // keeps that obligation visible at the two call sites instead of split
        // across a helper.
        return true;
    }

    // Phases 11, 12 and 17's contribution to the ONE declaration phase 10 makes
    // on the graph arm (NRI Phase 3, Task 9).
    //
    // WHY IT IS CALLED FROM PHASE 10 RATHER THAN FROM THOSE PHASES. On the
    // NVRHI arm each of the three is a separate GPU submission that can happen
    // at its own point in the frame, after the scene has already been rendered.
    // On this recorder they are NODES in one graph that is declared, compiled
    // and executed by a single RenderFrameOffscreen call -- so everything the
    // frame will contain has to be known BEFORE that call, and phase 10 is
    // where it is made. Moving the call earlier is not an option either: the
    // game HUD's draw data must be produced AFTER the scene submission, because
    // that is the order the plugin's SubmitRender and DrawUI run in on the
    // NVRHI arm and a plugin whose DrawUI mutates the scene must not land on a
    // different frame here. Phases 11, 12 and 17 keep their gates and each
    // points at this function.
    void EditorApp::ArmGraphViewportFrame(Arcane::NriGraphContext::FrameDesc& vp)
    {
        // NRI Phase 3, Task 13: `full` is the only editor golden stage that
        // carries the game HUD and the selection/pick outline -- both are
        // overlay content drawn on top of the scene, the same class of thing
        // the host HUD's own stage gate exists to mask (DeclareGraphFrame;
        // RuntimeFrame::RenderNvrhi's matching NVRHI-arm gate). Honoured only
        // in golden mode: an ordinary run's stage is always Full
        // (HostConfig::Parse refuses otherwise), so `goldenNonFull` folds to
        // false there and nothing below changes.
        //
        // TWO DIFFERENT MECHANISMS for the two overlays below, and the
        // asymmetry is deliberate -- see the pick/outline block's own comment
        // for why gating it HERE, at the call site, rather than in the shared
        // engine declaration (the way gameUi now is) is the correct, narrower
        // choice for THAT one.
        const bool goldenNonFull = m_config.GoldenMode() &&
                                   m_config.goldenStage != Arcane::GoldenStage::Full;

        // ---- phase 11's content: the game HUD ---------------------------
        // Play-mode only, exactly as that phase is, and through the SAME
        // helper the NVRHI arm calls. The draw data lives in the game context
        // until its next NewFrame -- a whole frame away -- which is what lets
        // the node copy the geometry at record time (FrameDesc::gameUi).
        if (BeginGameUiFrame())
        {
            // Same discard-vs-render split RuntimeFrame::RenderGraph applies
            // to the HOST HUD: `full` hands the draw data to the graph;
            // batch/post end the ImGui frame without rendering it, so a stage
            // golden is never masked by overlay content. This call site gates
            // TOO rather than leaning solely on DeclareGraphFrame's own
            // belt-and-braces recheck (RgFrameShape::gameUi, stage-gated as
            // of this task) -- the same "re-check so the two cannot drift
            // apart" reasoning the engine's own comment states for the host
            // HUD gate.
            if (goldenNonFull)
                m_gameImgui->EndFrameDiscard();
            else
                vp.gameUi = m_gameImgui->RenderToDrawData();
        }

        // ---- phases 12 + 17's content: the pick + outline chain ----------
        // THE HOVER, PINNED IN GOLDEN MODE (whole-branch review, C2). See
        // HoverLive()'s own comment: m_gameUi.inViewport is derived from the
        // LIVE pointer, so leaving it in the gate below made the editor's
        // `full` artifact a function of where the mouse happened to be.
        const bool hoverLive = HoverLive();
        // PHASE 12'S GATE, VERBATIM: Edit mode, and something to outline
        // (a selection, or a cursor in the viewport that might hover one).
        const bool wantOutline = !InPlayMode()
                              && (m_selection.HasSelection() || hoverLive);
        // ...and phase 17's: a click whose readback has not landed keeps the
        // chain declared even when nothing wants an outline, because the
        // readback node is the only thing that DRAINS the slot its copy went
        // into. Dropping the chain mid-flight would strand that slot's pending
        // flag until the chain happened to come back. DeferredPick::Busy() is
        // true from the click until the answer lands or is abandoned, which is
        // exactly the window this needs.
        const bool wantPick = m_deferredPick.Busy();
        // `goldenNonFull` suppresses the WHOLE chain (id pass + outline),
        // unconditionally -- unlike RgFrameShape::pickOutline itself, which
        // stays deliberately STAGE-INDEPENDENT at the engine level (its own
        // header comment: "a stage golden is taken without the probe, so the
        // two never meet in practice"). That reasoning covers --pick-probe on
        // the RUNTIME arm, which is NOT inert the way gameUi was:
        // RuntimeFrame::RenderGraph arms pickOutline whenever --pick-probe is
        // passed, REGARDLESS of --golden-stage, and there is no Parse-time
        // refusal against combining them. Gating shape.pickOutline in the
        // engine would therefore be a real behaviour change to that flag, not
        // a shape-visible-but-inert one -- so this decision is taken HERE,
        // narrowly, changing only what the EDITOR arms for its OWN golden
        // capture, rather than in DeclareGraphFrame.
        //
        // AN EARLIER VERSION OF THIS COMMENT CLAIMED wantOutline/wantPick WERE
        // "ALREADY FALSE" ON A SCRIPTED RUN, on the grounds that such a run
        // never issues a viewport click and never holds a selection. Both
        // halves are true and the conclusion was still wrong, which is the
        // whole-branch review's C2: HOVER needs neither. m_gameUi.inViewport is
        // re-derived from the LIVE pointer every frame (FrameInput), so with
        // the mouse resting over the Viewport panel a scripted run DID arm this
        // chain and DID outline whatever sat under the cursor -- the same
        // command line producing a different `full` artifact depending on where
        // the mouse was left. HoverLive() is what makes that impossible now,
        // for every stage; what remains here is the STAGE rule, which only has
        // to make "batch/post carry no outline" true when a genuine SELECTION
        // is held.
        //
        // THE D3C DECISION, NOW TAKEN (Task 13 follow-up): the editor's `full`
        // DOES script a selection, and that is the only reason this chain is
        // ever armed on a golden run. The first D3c capture proved why it had
        // to be: with hover pinned off and nothing selected, `full` armed no
        // outline nodes and came out byte-identical to `post` (one md5 across
        // editor-post-* and editor-* on both backends) -- a stage that provably
        // equals its predecessor is a dead gate, and the pick/JFA/composite
        // chain is the most editor-specific rendering there is.
        //
        // WHAT IS SELECTED, and where: PinGoldenViewport (phase 9, before the
        // camera push) selects the entity the id pass calls HIT-PROXY ID 1 --
        // the same handle the runtime's `--pick-probe` fabricates, derived from
        // CollectPickables itself so the NVRHI and graph arms cannot disagree
        // about which entity that is. See Viewport/GoldenViewPin.hpp.
        //
        // THE STAGE RULE BELOW IS WHAT KEEPS THAT HONEST: `goldenNonFull` still
        // suppresses the whole chain, so the scripted selection reaches `full`
        // ONLY. Its one other rendered consequence -- the transform gizmo,
        // which draws into the scene batch and would therefore have leaked into
        // `batch`/`post` -- is pinned off by GizmoLive() (EditorApp.hpp).
        if (goldenNonFull || (!wantOutline && !wantPick))
        {
            // Cleared rather than left stale: these are the spans the NEXT
            // armed frame will publish, and a leftover from three frames ago is
            // an id assignment that no longer matches any id pass.
            m_pickDrawables.clear();
            m_pickSelectedIds.clear();
            return;
        }

        // THE ONE EMITTER (PickEmit.hpp) -- a pure registry walk through the
        // same world->canvas transform the scene render just used, so the id
        // silhouettes register pixel-for-pixel with what was drawn. The k-th
        // entry IS hit-proxy id k+1. (Before NRI Phase 5a, Task 4 deleted
        // PickBuffer, PickBuffer::RenderIdPass drove the identical walk on
        // the NVRHI arm, through the same emitter.)
        const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
        m_pickDrawables.clear();
        Arcane::CollectPickables(m_runtime->Registry(), view, m_pickDrawables);

        // Every selected entity that made it into THIS frame's id pass, over
        // the same drawables that were just handed to the pick node rather
        // than over a table a previous RenderIdPass left behind (the NVRHI
        // arm's pick->PassIdOf(e) loop did the equivalent lookup before NRI
        // Phase 5a, Task 4 deleted PickBuffer). Empty in Play, which is how
        // "phase 12 does not run in Play" is expressed here: the seed finds
        // no selected silhouette and the composite discards every pixel.
        m_pickSelectedIds.clear();
        if (wantOutline)
        {
            m_pickSelectedIds.reserve(m_selection.Count());
            for (Astra::Entity e : m_selection.Entities())
            {
                const std::uint32_t id = Arcane::PickPassIdOf(m_pickDrawables, e);
                if (id != 0u)
                    m_pickSelectedIds.push_back(id);
            }
        }

        vp.pickOutline = true;
        vp.pickables   = m_pickDrawables;
        vp.selectedIds = m_pickSelectedIds;
        // The HOVER cursor: the viewport-local pointer when it is over the
        // viewport, and the (-1,-1) "no hover" sentinel otherwise -- including
        // in Play, where the outline must not light anything. (The same
        // sentinel the NVRHI arm's SelectionOutline::Params::cursorPx used,
        // before NRI Phase 5a, Task 4 deleted it.)
        vp.hoverPixel  = (wantOutline && hoverLive)
                       ? glm::ivec2((int)m_gameUi.viewportMouse.x, (int)m_gameUi.viewportMouse.y)
                       : glm::ivec2(-1, -1);

        // THE CLICK, if one has been waiting for a frame to rasterise it. The
        // id<->entity table handed over is THIS frame's -- the one the id pass
        // about to be declared will be built from, and therefore the only table
        // the id coming back can honestly be inverted through.
        vp.pickPixel  = glm::ivec2(0, 0);   // nothing to resolve: a texel nobody reads
        vp.pickTicket = 0;                  // ...and the label that says so (DeferredPick::Land refuses 0)
        if (m_deferredPick.State() == Arcane::Editor::DeferredPick::Phase::Armed)
        {
            std::vector<Astra::Entity> ordered;
            ordered.reserve(m_pickDrawables.size());
            for (const Arcane::PickDrawable& d : m_pickDrawables)
                ordered.push_back(d.entity);
            if (const auto request = m_deferredPick.TakeRequest(ordered))
            {
                vp.pickPixel  = request->pixel;
                vp.pickTicket = request->ticket;
            }
        }
    }

    // Phase 11: the game's own ImGui HUD, composited into the viewport texture.
    // Runs AFTER the scene render above and BEFORE the editor's BeginFrame below.
    // Game debug UI -> the plugin's OWN ImGui context, composited into the
    // viewport's output texture (Play only). Edit mode: not run (clean
    // viewport).
    //
    // ===== THE GRAPH ARM DOES THIS PHASE'S WORK IN PHASE 10 =====
    // (NRI Phase 3, Task 9. Read this before concluding the gate is a
    // leftover -- what it means CHANGED completely in that task.)
    //
    // Until Task 9 this gate was the only thing standing between a plugin
    // window and the editor's PER-PROJECT LAYOUT FILE: the graph arm had no
    // game ImGui context at all, so StageRenderBridge handed the plugin the
    // EDITOR's, whose io.IniFilename is that file. A DrawUIAll reached from
    // here would have persisted plugin windows into the user's layout
    // permanently -- the imgui.ini veto class. Task 8's rule was therefore
    // "this gate may come out only TOGETHER WITH a separate game-UI
    // context", and that is what happened: the graph arm now builds the
    // same OffscreenImGuiLayer (device-less flavor, same io.IniFilename =
    // nullptr, same pinning), StageRenderBridge's SetImGui is
    // unconditional again, FrameInput's fs.gameUiClaims is unconditional
    // again, and DrawUIAll runs on both arms. ONE CHANGE, as required.
    //
    // The gate SURVIVES it, for an unrelated and purely structural reason:
    // on the graph recorder the HUD is a NODE inside the viewport frame,
    // and that frame is declared, compiled and executed by phase 10's
    // single RenderFrameOffscreen call -- which has already happened by the
    // time control reaches here. ArmGraphViewportFrame (above) is where
    // this phase's content is produced on that arm, through the SAME
    // BeginGameUiFrame helper, at the same point relative to the scene
    // submission.
    //
    // NRI Phase 5a, Task 4 deleted the NVRHI arm this function used to carry
    // below an `if (GraphMode())` early return (the GPU composite of the game
    // HUD into m_viewportTargets.canvas's output texture, a direct nvrhi
    // call) -- that predicate was unconditionally true, so the arm was already
    // unreachable. Task 11a removed the predicate, which leaves this function
    // EMPTY. It and its one caller are inert scaffolding; retiring them is
    // outside a predicate collapse and is carried in that task's report.
    void EditorApp::CompositeGameUi()
    {
    }

    // Phase 12: Edit-mode selection/hover outline. Occupies the same slot as the
    // Play-only game-imgui composite above -- the two are mutually exclusive by
    // mode -- and must stay after the scene render, before the editor ImGui pass.
    //
    // THE GRAPH ARM DOES THIS PHASE'S WORK IN PHASE 10 (NRI Phase 3, Task 9),
    // and always has: the picker and the selection outline are NODES in the
    // viewport context's own frame (PickNode/OutlineNode), armed through
    // FrameDesc::pickOutline/pickables/selectedIds/hoverPixel -- and that
    // frame is declared, compiled and executed by phase 10's single
    // RenderFrameOffscreen call, which has already happened by the time
    // control reaches here. ArmGraphViewportFrame carries this phase's gate
    // VERBATIM (Edit mode, and a selection or a cursor in the viewport) and
    // maps m_selection through the same k+1 id assignment PickBuffer used to.
    //
    // NRI Phase 5a, Task 4 deleted the NVRHI arm this function used to carry
    // below an `if (GraphMode())` early return (PickBuffer::RenderIdPass +
    // SelectionOutline::Render, direct nvrhi calls) -- that predicate was
    // unconditionally true, so the arm was already unreachable. Task 11a
    // removed the predicate, which leaves this function EMPTY. It and its one
    // caller are inert scaffolding; retiring them is outside a predicate
    // collapse and is carried in that task's report.
    void EditorApp::RenderSelectionOutline()
    {
    }

    // Phase 13: the editor's own document upkeep. The compile pump that used to
    // live here -- Poll/Drain, the scene sweep, the table publish and the clock
    // advance -- moved into SceneRenderResolver::Refresh (phase 9) when
    // resolution was lifted into the engine so both hosts share it. Open
    // documents still get first refusal on every drained result through the
    // consumeFirst hook installed in EditorApp::Init, so the process still has
    // exactly ONE drain site. What stays is what only an editor has: the
    // external-edit watcher and the per-document tick (each document renders
    // its own preview into its own offscreen NriGraphContext).
    void EditorApp::PumpEditorDocuments()
    {
        // FIRST, and before anything can close another document: destroy the
        // preview vehicles that LAST frame's closes handed over (NRI Phase 3,
        // Task 11). By now the chrome frame whose draw lists named them has
        // been recorded AND submitted, which is the whole condition the
        // deferral exists to satisfy. Same idiom, one level up, as
        // ShaderEditorDocument's own m_nodePreviewRetired.clear() at the top of
        // Tick.
        DrainRetiredDocPreviews();
        PollMaterialWatch();   // external .arcmat edits (~1 Hz mtime sweep)
        m_documents.TickAll(m_gameUi.frameDt);
    }

    // Phase 14: the editor ImGui pass -- dockspace, menus, dialogs, panels.
    // Everything it draws samples the viewport texture the render phases above
    // already produced. The Ctrl+N/O/S edges raised in the input phase are
    // folded into this frame's MenuRequests here, so keybind and menu item
    // cannot drift apart.
    void EditorApp::DrawEditorUi(LoopState& ls, const FrameState& fs)
    {
        // ImGui: editor shell -- full-viewport dockspace + Sim toolbar + Console panel
        // + the Viewport panel showing the scene texture just rendered above.
        UpdateWindowTitle();   // project + scene name + unsaved marker
        m_gpu->Imgui().BeginFrame();
        Arcane::Editor::MenuRequests menuReq;
        // Build -> Rebuild Game Module gating inputs: the menu greys the item
        // while playing (UE model), while a build runs, and for a project
        // with no gameModule (content-only, or none open).
        const Arcane::Project* menuProj = m_runtime->CurrentProject();
        const bool hasGameModule = menuProj && !menuProj->Manifest().gameModule.empty();
        Arcane::Editor::BeginDockSpace(*m_undo, menuReq, m_scene.IsDirty(*m_undo),
                                       InPlayMode(),
                                       m_moduleBuild.Running(), hasGameModule,
                                       m_panelVis,
                                       m_selection.HasSelection(),
                                       m_assetBrowser.selected.IsValid(),
                                       &m_recents.projects,
                                       &m_recents.scenes);
        // Play button's SeparateWindow branch: the toolbar only REPORTS the
        // click (same "panel reports, app performs" split as ViewportPanelResult);
        // the readiness checks live in SceneSession::Request's park condition
        // (dirty OR never-saved) and DoLaunchStandalone keeps only a loud
        // backstop before the spawn.
        if (Arcane::Editor::DrawSimTimeToolbar(m_play, *m_runtime,
                                               m_plugin ? m_plugin->Vtable() : nullptr, m_playMode,
                                               ToolbarLogoTextureId()))
        {
            // Mid-ImGui-pass site -> the deferral convention (SceneSession::Request's
            // comment): clean+saved acts next frame top; dirty/never-saved parks
            // behind the shared confirm modal.
            if (m_scene.Request(Arcane::Editor::SceneIntent::LaunchStandalone, {}, *m_undo))
                ls.sceneAction = { Arcane::Editor::SceneIntent::LaunchStandalone, {} };
        }
        // The toolbar's Play/Stop click is the only mid-frame PlayMode flip point
        // (sec 1's rule): re-derive so this frame's consume blocks and panels see
        // the true state, not last frame's.
        //
        // A FLIP HERE ALSO REPLACES THE SCENE (NRI Phase 3, Task 9): both
        // PlaySession::Play and ::Stop go through Runtime::Snapshot/
        // RestoreRegistry, which destroys and replaces the registry OBJECT --
        // so a graph-arm click-pick in flight names entities of a registry that
        // no longer exists. Noted here rather than inside PlaySession because
        // this is the ONE site that can flip the mode, and because the same
        // line already exists to say exactly that.
        if (m_editBinding.editMode == InPlayMode())
            NoteSceneReplaced();
        m_editBinding.editMode = !InPlayMode();
        Arcane::Editor::EndDockSpace(menuReq.resetLayout);
        if (menuReq.resetLayout)
            m_panelVis = Arcane::Editor::PanelVisibility{};   // reset re-shows everything

        ConsumeMenuRequests(menuReq, fs, ls);

        Arcane::Editor::AssetBrowserActions browserActions;
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Assets))
            browserActions = Arcane::Editor::DrawAssetBrowserPanel(
                m_assetBrowser, m_runtime->CurrentProject(), m_documents,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Assets));
        ConsumeBrowserActions(browserActions, ls);

        if (static_cast<std::size_t>(m_consoleDiag.ui.lineCap) != m_consoleDiag.console.Capacity())
            m_consoleDiag.console.SetCapacity(static_cast<std::size_t>(m_consoleDiag.ui.lineCap));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Console))
            Arcane::Editor::DrawConsolePanel(m_consoleDiag.console, m_consoleDiag.ui,
                m_panelVis.OpenFlag(Arcane::Editor::PanelId::Console));

        // Problems panel: current diagnostic STATE (Console above is the
        // append-only log stream). A clicked row's locator is routed here,
        // mid-DrawEditorUi -- the same place browserActions.createSpriteFrom
        // above already opens documents synchronously; only project/scene
        // teardown needs the frame-boundary deferral this function's other
        // effects use.
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Problems))
            if (const std::optional<Arcane::DiagLocator> hit =
                    Arcane::Editor::DrawProblemsPanel(m_consoleDiag.store, m_consoleDiag.problemsUi,
                        m_panelVis.OpenFlag(Arcane::Editor::PanelId::Problems)))
                RouteLocator(*hit);

        // The Material panel draws BEFORE the documents, and that order is
        // load-bearing rather than incidental: the panel's param rows and the
        // document's graph widgets open gestures against the SAME
        // EditGesture::GestureState, and each scope's ScopeGuard closes an
        // abandoned one when it destructs. Drawing the panel first leaves
        // ShaderEditorDocument::Draw's guard as the LAST one to run each frame,
        // so a gesture the panel opened is never force-closed by the document
        // before the panel's own EndOnDeactivate has had its say.
        Arcane::Editor::DrawMaterialPanel(ResolveActiveMaterialDoc());

        // New documents tab into the Viewport's node (captured last frame).
        m_documents.DrawAll(m_viewportDockId);
    }

    void EditorApp::ConsumeMenuRequests(Arcane::Editor::MenuRequests& menuReq,
                                        const FrameState& fs, LoopState& ls)
    {
        // Bare interactive launch: raise the picker as if the user had clicked
        // File -> Open Project, once. Routed through menuReq (rather than
        // calling the dialog directly) so there is exactly ONE launch site and
        // the cold-start path cannot drift from the menu path.
        if (m_raiseOpenProjectOnStart)
        {
            m_raiseOpenProjectOnStart = false;
            menuReq.openProject       = true;
        }
        if (menuReq.openProject)
            m_gpu->Win().ShowOpenFileDialog(&EditorApp::PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.projectOpen, m_dialogs.projectOpen.Arm() },
                "Arcane Project", "arcproj");
        // Open Recent lands in the SAME slot the file dialog's callback fills,
        // so it flows through ConsumeProjectDialogResult and inherits every
        // guard the menu path already has -- the unsaved-scene confirm, the
        // rival-editor lock, the ABI gate, the failure modal. A second open
        // path would have to re-earn all of them.
        if (!menuReq.openRecentPath.empty())
            m_dialogs.projectOpen.Stash(m_dialogs.projectOpen.Arm(), menuReq.openRecentPath);
        // A picked recent scene lands in the SAME slot the Open Scene dialog's
        // callback fills, so it flows through ConsumeSceneDialogResults and
        // inherits the unsaved-scene guard -- a second open path would have to
        // re-earn it.
        if (!menuReq.openRecentScenePath.empty())
            m_dialogs.sceneOpen.Stash(m_dialogs.sceneOpen.Arm(), menuReq.openRecentScenePath);
        // Refresh on the RISING EDGE of the File menu only. That is what lets a
        // project opened in the Hub while this editor runs appear without a
        // restart, without paying a file read plus a stat per project on every
        // frame of a session where the menu is never touched.
        if (menuReq.fileMenuOpen && !m_recents.fileMenuWasOpen)
            m_recents.RefreshAll(m_runtime->CurrentProject());
        m_recents.fileMenuWasOpen = menuReq.fileMenuOpen;
        // Build -> Rebuild Game Module: spawn the worker right here (no
        // dialog, no teardown -- nothing about starting a build needs the
        // frame-boundary deferral the scene/project requests above use). The
        // finish-side effects all live in PollModuleBuild.
        if (menuReq.rebuildModule)
            StartModuleRebuild();
#if !defined(ARCANE_DIST)
        // Build -> Diagnostics -> Crash GPU: fired RIGHT HERE, mid-ImGui-pass,
        // rather than deferred to a frame boundary the way the scene/project
        // requests above are. It needs no teardown and no dialog, and this
        // point in the frame is safe for it: no graph frame has been declared
        // yet, so FireDeliberateGpuFault's own one-off command buffer cannot
        // collide with anything already recording. Deferring would also make
        // the capture LESS representative rather than more: a device lost
        // while the host is mid-frame is exactly the shape this arc exists
        // to survive.
        if (menuReq.crashGpu)
            FireDeliberateGpuFault();
#endif
        // Edit -> selection ops. Pure selection state -- no undo step (UE
        // does not undo selection either).
        if (menuReq.deselectAll)
            m_selection.Clear();
        if (menuReq.selectAll || menuReq.invertSelection)
        {
            const std::vector<Astra::Entity> all =
                Arcane::Editor::CollectSceneEntities(m_runtime->Registry());
            const std::vector<Astra::Entity> next = menuReq.selectAll
                ? all
                : Arcane::Editor::InvertSelectionSet(all, m_selection);
            m_selection.Clear();
            m_selection.AddRange(next, next.empty() ? Astra::Entity::Invalid()
                                                    : next.back());
        }
        // Edit -> Rename / Delete: the Outliner's own F2/Del code paths
        // (promoted in EditorPanels.hpp) -- menu and keybind cannot drift.
        if (menuReq.renameSelected && m_selection.HasSelection())
        {
            if (const Arcane::Identity* info =
                    m_runtime->Registry().GetComponent<Arcane::Identity>(m_selection.Primary()))
            {
                // The rename box lives in the Outliner: un-hide it and pull its
                // tab forward so the edit box appears where the user can see it,
                // instead of latching a rename that ambushes them whenever the
                // panel next shows (final-review Important 2).
                m_panelVis.visible[static_cast<std::size_t>(Arcane::Editor::PanelId::Outliner)] = true;
                Arcane::Editor::SelectDockTab("Outliner");
                Arcane::Editor::BeginRename(m_outliner, m_selection.Primary(), info->name);
            }
        }
        if (menuReq.deleteSelected)
            Arcane::Editor::DeleteSelection(m_runtime->Registry(), m_selection,
                                            *m_undo, m_editBinding);

        // Edit -> clipboard shortcuts (Ctrl+X/C/V/D, raised in the input phase
        // above). Folded in HERE, before the clipboard consume block below reads
        // menuReq -- same fold-in shape as the Ctrl+N/O/S scene shortcuts fold
        // further down, just earlier in the frame so the request this edge
        // raises cannot lag a frame behind its own keypress.
        menuReq.cutSelection       |= fs.scCut;
        menuReq.copySelection      |= fs.scCopy;
        menuReq.paste              |= fs.scPaste;
        menuReq.duplicateSelection |= fs.scDuplicate;
        // Edit -> clipboard (spec II.B), delegated to the shared functions
        // promoted in EditorPanels.cpp/.hpp -- the menu-bar consume, the
        // keybinds (folded in above), and the Outliner's context menus all
        // route through the same four, so they cannot drift.
        if (menuReq.copySelection)
            Arcane::Editor::CopySelectionToClipboard(m_runtime->Registry(), m_selection);
        if (menuReq.cutSelection)
            Arcane::Editor::CutSelection(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);
        if (menuReq.paste)
            Arcane::Editor::PasteFromClipboard(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);
        if (menuReq.duplicateSelection)
            Arcane::Editor::DuplicateSelection(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);

        // File -> Save All: scene first (never-saved routes through the Save
        // As dialog, exactly like Save), then every dirty document in place.
        if (menuReq.saveAll)
        {
            if (m_scene.IsDirty(*m_undo))
            {
                if (!m_scene.Path().empty()) DoSaveScene(m_scene.Path());
                else                         ShowSceneSaveDialog();
            }
            if (const std::size_t failed = m_documents.SaveAllDirty())
                ARC_ERROR("Save All: {} document save(s) failed (see Console)", failed);
        }
        // File -> Exit: the SAME SceneIntent::Exit flow the OS quit uses --
        // dirty scene parks behind the confirm modal; a clean exit defers to
        // the top of the next frame like every other scene action.
        if (menuReq.exitEditor &&
            m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
        {
            ls.sceneAction = { Arcane::Editor::SceneIntent::Exit, {} };
        }

        // Assets -> Show in Explorer / Copy Path, on the browser's tracked row.
        if ((menuReq.showInExplorer || menuReq.copyAssetPath) &&
            m_assetBrowser.selected.IsValid())
        {
            AssetPathAction(m_runtime->CurrentProject(), m_assetBrowser.selected,
                            menuReq.showInExplorer, menuReq.copyAssetPath);
        }

        if (menuReq.newMaterial || menuReq.openMaterial)
        {
            // Material dialogs start in the project's Content/ (the only place
            // a saved asset can register + resolve by GUID); no project = OS default.
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const std::string contentDir =
                proj ? (proj->Root() / "Content").string() : std::string();
            const char* defaultPath = contentDir.empty() ? nullptr : contentDir.c_str();
            if (menuReq.newMaterial)
                m_gpu->Win().ShowSaveFileDialog(&EditorApp::PathPickedThunk,
                    new PathDialogRequest{ &m_dialogs.materialNew, m_dialogs.materialNew.Arm() },
                    "Arcane Material", "arcmat", defaultPath);
            if (menuReq.openMaterial)
                m_gpu->Win().ShowOpenFileDialog(&EditorApp::PathPickedThunk,
                    new PathDialogRequest{ &m_dialogs.materialOpen, m_dialogs.materialOpen.Arm() },
                    "Arcane Material", "arcmat", defaultPath);
        }

        // Scene menu items + their Ctrl+N/O/S shortcuts (raised in the input
        // phase above). Folded together here so both routes hit the same guard.
        menuReq.newScene  |= fs.scNewScene;
        menuReq.openScene |= fs.scOpenScene;
        // Ctrl+S means "save what I am looking at". An open asset document binds
        // its own Ctrl+S with ImGui::Shortcut, which routes to the focused
        // window by itself -- but THIS keybind is raw scancodes off the input
        // snapshot and knows nothing about that routing, so without the check
        // one keypress inside a material or sprite document would save the
        // document AND the scene. Asking the host who is focused is what keeps
        // exactly one of them firing.
        //
        // Only the SHORTCUT routes. The File menu's Save Scene item folds in
        // below unconditionally: an item that names the scene saves the scene,
        // whatever happens to hold focus.
        const bool docOwnsSave =
            fs.scSaveScene && m_documents.FocusedDoc() != nullptr;
        menuReq.saveScene |= (fs.scSaveScene && !docOwnsSave);

        if (menuReq.newScene &&
            m_scene.Request(Arcane::Editor::SceneIntent::NewScene, {}, *m_undo))
        {
            // Deferred to the top of the next frame for the same reason the
            // confirm modal's answer is -- see sceneAction.
            ls.sceneAction = { Arcane::Editor::SceneIntent::NewScene, {} };
        }
        if (menuReq.openScene)
        {
            const std::string dir = SceneDialogDir();
            m_gpu->Win().ShowOpenFileDialog(&EditorApp::PathPickedThunk,
                new PathDialogRequest{ &m_dialogs.sceneOpen, m_dialogs.sceneOpen.Arm() },
                "Arcane Scene", "arcscene",
                dir.empty() ? nullptr : dir.c_str());
        }
        // Save on a never-saved scene IS Save As -- otherwise the item would look
        // enabled and do nothing.
        if (menuReq.saveScene && !m_scene.Path().empty())
            DoSaveScene(m_scene.Path());
        if (menuReq.saveSceneAs || (menuReq.saveScene && m_scene.Path().empty()))
            ShowSceneSaveDialog();
    }

    void EditorApp::ConsumeBrowserActions(const Arcane::Editor::AssetBrowserActions& browserActions,
                                          LoopState& ls)
    {
        if (browserActions.createInstanceOf.IsValid())
        {
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const std::string contentDir =
                proj ? (proj->Root() / "Content").string() : std::string();
            m_gpu->Win().ShowSaveFileDialog(&EditorApp::InstancePickedThunk,
                new InstanceDialogRequest{ &m_dialogs.instanceNew,
                                           m_dialogs.instanceNew.Arm(),
                                           browserActions.createInstanceOf },
                "Arcane Material", "arcmat",
                contentDir.empty() ? nullptr : contentDir.c_str());
        }
        if (browserActions.createSpriteFrom.IsValid())
        {
            // Browser-initiated mint OPENS the new sprite document (the user
            // right-clicked a specific texture asking for exactly this); the
            // Inspector's texture-drop auto-mint below does NOT -- that one is
            // a means to filling a field, not a request to edit the sprite.
            if (const Arcane::Guid minted =
                    MintOrReuseSpriteForTexture(browserActions.createSpriteFrom);
                minted.IsValid())
            {
                if (const Arcane::Project* proj = m_runtime->CurrentProject())
                    if (const auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(minted)))
                        m_documents.OpenPath(*p);
            }
        }
        if (!browserActions.openScene.empty())
        {
            // A scene double-clicked in the browser is not a document -- it
            // replaces the editing session, so it goes through the same
            // SceneSession guard as File -> Open Scene. Request() is pure
            // state and safe to call from here; the load itself is deferred
            // to sceneAction for the same reason menuReq's scene items above
            // are (this call site is mid-ImGui-pass, after BeginDockSpace).
            // A parked (dirty-scene) result is picked up by the "Unsaved
            // Scene" modal below, whose Save/Discard branches already set
            // sceneAction via TakePending().
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene,
                                browserActions.openScene, *m_undo))
                ls.sceneAction = { Arcane::Editor::SceneIntent::OpenScene,
                                   browserActions.openScene };
        }
        if (browserActions.setBootScene.IsValid())
        {
            // No unsaved-changes guard: this only rewrites the project
            // manifest and does not touch the live registry or session.
            if (m_runtime->SetProjectBootScene(browserActions.setBootScene))
                ARC_INFO("Boot scene set to {}", browserActions.setBootScene.ToString());
            else
                m_modalErrors.Push("Scene Error", "Could not write the project's boot scene (see Console).");
        }
        // Row context menu parity for Show in Explorer / Copy Path (Part 3):
        // the SAME helper the menu-bar route above uses, on whichever row
        // the browser's own popup was just opened against.
        if (browserActions.showInExplorer.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), browserActions.showInExplorer, true, false);
        if (browserActions.copyPath.IsValid())
            AssetPathAction(m_runtime->CurrentProject(), browserActions.copyPath, false, true);
    }

    Arcane::Editor::ShaderEditorDocument* EditorApp::ResolveActiveMaterialDoc()
    {
        // Resolved from the guid every frame (documents are destroyed
        // synchronously on close), with a fallback to any open material
        // document so the panel is never empty while one exists -- e.g. after
        // the tracked document is closed and another remains.
        Arcane::Editor::ShaderEditorDocument* activeMat = nullptr;
        if (m_activeMaterialGuid.IsValid())
            activeMat = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(
                m_documents.FindByGuid(m_activeMaterialGuid));
        if (!activeMat)
        {
            m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
            {
                if (!activeMat)
                    activeMat = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d);
            });
            if (activeMat)
                m_activeMaterialGuid = activeMat->AssetGuid();
        }
        return activeMat;
    }

    // Phase 15: the modals. Opened and drawn at DOCKSPACE level, outside any
    // panel window -- that is why they live here and not inside a panel draw.
    void EditorApp::DrawModals(LoopState& ls)
    {
        // ONE error modal, FIFO off the queue (architecture pass sec 7). The title
        // is per-error so it stays honest about which action failed; Pop advances
        // to the next queued error on the following frame.
        if (const Arcane::Editor::ModalError* err = m_modalErrors.Front())
        {
            if (!ImGui::IsPopupOpen(err->title.c_str()))
                ImGui::OpenPopup(err->title.c_str());
            if (ImGui::BeginPopupModal(err->title.c_str(), nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextUnformatted(err->message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(120, 0)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
                {
                    m_modalErrors.Pop();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // Unsaved-scene confirm. SceneSession parked the intent (New Scene, Open
        // Scene, Open Project, Exit, LaunchStandalone) because the scene is dirty
        // (or, for LaunchStandalone only, never saved); this popup is where it is
        // answered. Same re-arm shape as the modal above. Computed once: every
        // branch below reads the same snapshot, so a TakePending() mid-block
        // cannot change what the rest of the frame's UI thinks is pending.
        const Arcane::Editor::SceneIntent pending = m_scene.Pending();
        const bool isLaunch = (pending == Arcane::Editor::SceneIntent::LaunchStandalone);
        if (pending != Arcane::Editor::SceneIntent::None &&
            !ImGui::IsPopupOpen("Unsaved Scene"))
            ImGui::OpenPopup("Unsaved Scene");
        if (ImGui::BeginPopupModal("Unsaved Scene", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (pending == Arcane::Editor::SceneIntent::None)
            {
                // The intent was taken OUTSIDE this popup: the Save button's
                // never-saved branch launches a file dialog and leaves the popup
                // up, and the save (and with it the action) lands at the top of a
                // later frame. Nothing left to ask about. Also covers a parked
                // LaunchStandalone's dialog-in-flight frames the same way.
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ImGui::TextUnformatted((isLaunch
                    ? "'" + m_scene.DisplayName() +
                      "' must be saved before playing in a separate window."
                    : "'" + m_scene.DisplayName() + "' has unsaved changes.").c_str());
                ImGui::Separator();
                if (ImGui::Button(isLaunch ? "Save and Play" : "Save",
                                  ImVec2(isLaunch ? 140.f : 90.f, 0)))
                {
                    // Stop Play BEFORE saving, in both branches below. Stop
                    // restores the pre-Play snapshot, which IS the authored
                    // state -- so what reaches disk is exactly what the user
                    // believes they are saving, and DoSaveScene's Play refusal
                    // (which would otherwise turn this button into an error
                    // popup) is satisfied. Not a surprising side effect: no
                    // intent parkable here leaves Play running anyway -- New
                    // Scene, Open Scene and Open Project all stop it through
                    // ClearSceneReferences, Exit ends the process, and a parked
                    // LaunchStandalone only reaches here already saved-or-dirty,
                    // never mid-Play in a way DoLaunchStandalone itself needs.
                    if (InPlayMode())
                        m_play.Stop(*m_runtime, m_plugin ? m_plugin->Vtable() : nullptr);
                    if (m_scene.Path().empty())
                    {
                        // Never saved: this needs a filename first. The intent
                        // stays parked and this popup stays up until the dialog
                        // resolves, so cancelling the dialog cancels only the
                        // save, not the action behind it.
                        ShowSceneSaveDialog();
                    }
                    else if (DoSaveScene(m_scene.Path()))
                    {
                        ls.sceneAction = m_scene.TakePending();
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        // The save failed (m_modalErrors carries why). Drop the
                        // action rather than proceed -- proceeding would discard
                        // exactly the work the save was meant to preserve.
                        m_scene.ClearPending();
                        ImGui::CloseCurrentPopup();
                    }
                }
                // Discard: hidden for a launch -- the standalone runtime reads
                // the scene from DISK, so "discard and play" would run a stale
                // file.
                if (!isLaunch)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Discard", ImVec2(90, 0)))
                    {
                        ls.sceneAction = m_scene.TakePending();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(90, 0)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    m_scene.ClearPending();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    // Phase 16: the Viewport panel itself. It samples the texture the render
    // phases produced, and publishes the rect/active/size the NEXT frame's
    // input phase and resize phase read (the deliberate one-frame lag).
    void EditorApp::DrawViewportPanelPhase(FrameState& fs)
    {
        // THE TEXTURE. 0 IS A REAL, EXPECTED VALUE and is the "draw nothing"
        // signal: a ResizeOffscreen whose replacement could not be created
        // leaves the output null on purpose. DrawViewportPanel already skips
        // its ImGui::Image at 0 (EditorPanels.cpp) and still publishes
        // dockId/desiredW/desiredH, so the panel keeps measuring and the next
        // non-zero size heals it through phase 8.
        //
        // NRI Phase 5a, Task 4 deleted the NVRHI fallback this ternary used to
        // carry (`m_viewportTargets.canvas ? canvas->TextureId() : 0`),
        // reachable when `graph` was null -- a project switch whose rebuild
        // fails (SwitchProject's "render_bridge"), which sets m_requestExit,
        // read at the TOP of the next frame so THIS frame still runs phase 16.
        // 0 is exactly the value that fallback produced on the graph arm (no
        // `canvas` ever existed there), so this is unchanged behaviour, not a
        // narrower answer.
        const std::uint64_t vpTexture = m_viewportTargets.graph
                                          ? m_viewportTargets.graph->OffscreenTextureId()
                                          : 0;
        fs.vp = Arcane::Editor::DrawViewportPanel(vpTexture,
                                            ViewportWidth(), ViewportHeight(),
                                            m_gizmoEnabled, m_gizmoMode, m_gizmoSpace,
                                            /*showToolOverlay=*/!InPlayMode());
        m_viewportDockId = fs.vp.dockId;
        m_viewportTargets.pendingW = fs.vp.desiredW;
        m_viewportTargets.pendingH = fs.vp.desiredH;
        m_viewportRect     = fs.vp.imageRect;
        m_viewportActive   = Arcane::Editor::SceneInputActive(fs.vp.hovered, fs.vp.focused);
    }

    // Phase 16b: center-tab -> side-panel focus follow. Runs after BOTH the
    // documents (DrawEditorUi) and the Viewport panel have drawn, because it
    // consumes the one-frame "became the visible tab" edges they publish.
    //
    // TRANSITIONS ONLY, and that is the whole design: the edges come from
    // ImGui::IsWindowAppearing (imgui.cpp:9236-9240 -> window->Appearing, which
    // imgui.cpp:7905-7907 raises when a docked window's tab becomes visible
    // after being hidden), which is true for exactly one frame. A per-frame
    // "focus whatever matches" would re-select the tab every frame and make it
    // impossible to click the other one -- here, clicking Inspector while a
    // material document is active raises no edge, so the click STICKS until the
    // center tab actually changes again.
    //
    // The action is TAB SELECTION, NOT focus -- SelectDockTab, which parks
    // NextSelectedTabId on the side node's tab bar. This split is a bug fix,
    // not a preference: SetWindowFocus on a SIDE panel moves g.NavWindow, and
    // because a dock node applies g.NavWindow back as its own selection every
    // frame (imgui.cpp:19611-19613), one such call late in the frame silently
    // undid the CENTER node's adoption of a freshly-opened document -- the
    // document window focuses itself in Begin (imgui.cpp:8320-8326 sets
    // want_focus for an appearing window, :8617 calls FocusWindow), and this
    // phase, running afterwards, was overwriting it. Center = real focus
    // (DocumentHost::DrawAll's SetNextWindowFocus); side = tab selection.
    void EditorApp::SyncCenterTabFocus(const FrameState& fs)
    {
        Arcane::Editor::ShaderEditorDocument* becameActive = nullptr;
        std::size_t materialDocs = 0;
        m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
        {
            auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d);
            if (!doc)
                return;
            ++materialDocs;
            if (doc->TabBecameVisible())
                becameActive = doc;
        });

        if (becameActive)
        {
            // A material document opened, or its tab was selected. TAB
            // SELECTION only -- the center document owns keyboard focus (it is
            // what you type into), and SetWindowFocus here would take it back.
            m_activeMaterialGuid = becameActive->AssetGuid();
            Arcane::Editor::SelectDockTab("Material");
        }
        else if (fs.vp.appearing)
        {
            // The scene became the active center tab. The Viewport keeps focus;
            // only the side node's visible tab changes.
            Arcane::Editor::SelectDockTab("Inspector");
        }
        else if (materialDocs == 0 && m_materialDocCount > 0)
        {
            // The last material document closed, so the Material window stops
            // being submitted next frame and its tab disappears. Hand the side
            // node's selection to the Inspector explicitly rather than leaving
            // it on a tab that is about to vanish. Also the only branch that
            // fires for a FLOATING document, which leaves the center tab (and
            // so the Viewport's appearing edge) untouched.
            m_activeMaterialGuid = Arcane::Guid::Nil();
            Arcane::Editor::SelectDockTab("Inspector");
        }
        m_materialDocCount = materialDocs;
    }

    // Phase 17: click-pick. Must stay after the Viewport panel above (it reads
    // that panel's click) and after the input phase (it reads m_gizmoCapturedClick
    // and gameUiClaims from it).
    void EditorApp::HandleViewportPick(const FrameState& fs)
    {
        // Viewport click-pick: GPU hit-proxy. `view` is the SAME world->canvas
        // transform the scene render uses (m_runtime's camera ==
        // RenderContext2D), so the id silhouettes the graph's PickNode
        // rasterizes register pixel-for-pixel with what is drawn. An invalid
        // result (background / outside the viewport) clears the selection.
        // Suppressed when this frame's click was already consumed by the gizmo
        // (pressed/released a handle) or a drag is still in progress -- otherwise
        // clicking/using a handle would also re-pick and disturb the selection.
        // Also suppressed when the game's viewport debug UI claimed the pointer
        // this frame (gameUiClaims, computed in the input phase above) so a click
        // on a game HUD widget does not also re-pick the editor selection.
        //
        // ============ THE GRAPH ARM'S HALF (NRI Phase 3, Task 9) ============
        // NOT a like-for-like swap with the NVRHI arm's synchronous
        // PickBuffer::Pick (render the id pass on demand, waitForIdle, read
        // one texel, answer in this statement) -- that arm is deleted (NRI
        // Phase 5a, Task 4), and this was the one phase where the difference
        // showed in the editor's BEHAVIOUR rather than only in its plumbing.
        // The graph's readback is a graph COPY node whose value lands
        // kSwapchainFramesInFlight frames later, so the click is ARMED here
        // and the resulting Select/Toggle/Clear is APPLIED here on a later
        // frame. The plan's reconciliation 5 accepted that latency, and this
        // deferred shape is the only pick path left.
        //
        // TWO STEPS, AND THE ORDER IS DELIBERATE: apply what has landed FIRST,
        // then arm this frame's click. The other order would let a fresh click
        // discard an answer that had already arrived.
        //
        // THE FOUR GUARD CONDITIONS BELOW MATCHED THE NVRHI ARM'S, VERBATIM,
        // for as long as that arm existed -- same expression, same order, same
        // meaning (fs.vp.clicked, not a gizmo click, not mid-drag, and the
        // game HUD did not claim the pointer). The last of those became
        // honest again only because Task 9 restored the game context; while
        // it read `false` unconditionally neither arm could have had a
        // faithful pick.
        // ---- STEP 1: a landed readback -----------------------------
        // ProbeResult does NOT self-clear (it reports the same pair every
        // frame until the next drain), so DeferredPick::Land is what makes
        // consumption exactly-once -- it refuses anything but the ticket
        // outstanding, and returns to Idle whether or not the answer
        // survived its staleness checks.
        if (m_viewportTargets.graph)
        {
            if (const auto probe = m_viewportTargets.graph->ProbeResult())
            {
                if (const auto hit = m_deferredPick.Land(probe->ticket, probe->id,
                                                         m_sceneEpoch, InPlayMode()))
                {
                    // A THIRD staleness check, on top of Land's epoch and
                    // play-mode ones: the entity must still exist in the
                    // LIVE registry. The retained table can name an entity
                    // that was deleted (Delete key, an undo) without the
                    // scene having been REPLACED, which is a case no epoch
                    // can see. Dropping it is the safe direction -- note it
                    // is NOT folded into the miss branch, because a stale
                    // hit is "we do not know", where a miss is the positive
                    // statement "the user clicked background".
                    if (hit->entity.IsValid())
                    {
                        if (m_runtime->Registry().IsValid(hit->entity))
                        {
                            if (hit->ctrlHeld)
                                m_selection.Toggle(hit->entity);
                            else
                                m_selection.Select(hit->entity);
                        }
                    }
                    else if (!hit->ctrlHeld)
                    {
                        // Ctrl+click on empty space is a miss, not a
                        // deselect-all -- the NVRHI branch's rule, and the
                        // ctrl state is the one captured AT THE CLICK
                        // rather than whatever is held now.
                        m_selection.Clear();
                    }
                }
            }
            // Bounded, and loud if it ever fires: the readback is
            // guaranteed to drain after kSwapchainFramesInFlight RENDERED
            // frames, and ArmGraphViewportFrame keeps the chain declared
            // for exactly as long as this is Busy() -- so the only way to
            // exhaust the budget is a very long run of Skipped frames (a
            // collapsed Viewport panel). Giving up beats a state machine
            // that never returns to Idle.
            if (m_deferredPick.TickAndMaybeAbandon())
            {
                ARC_WARN("Viewport pick: no readback landed within {} frames -- the click was "
                         "dropped (was the Viewport panel collapsed?)",
                         Arcane::Editor::DeferredPick::kMaxFramesInFlight);
            }
        }

        // ---- STEP 2: this frame's click ----------------------------
        // Arm replaces any outstanding request: the newest click wins, and
        // the older one's copy is dropped on arrival by its stale ticket.
        // The scene epoch and play mode recorded here describe the scene
        // the USER clicked on, which is what the landing compares against.
        if (fs.vp.clicked && !m_gizmoCapturedClick && !m_gizmoDrag.active && !fs.gameUiClaims)
        {
            m_deferredPick.Arm(glm::ivec2((int)fs.vp.clickLocalX, (int)fs.vp.clickLocalY),
                               fs.vp.ctrlHeld, m_sceneEpoch, InPlayMode());
        }
        // Kept verbatim from the `if (GraphMode())` block Task 11a unwrapped:
        // this was that block's own trailing return, and it is now simply the
        // function's last statement. What it used to skip -- a synchronous
        // PickBuffer::Pick against the SAME four guard conditions -- was the
        // NVRHI arm, deleted at NRI Phase 5a, Task 4; the predicate that
        // guarded it was unconditionally true, so nothing followed it even
        // then.
        return;
    }

    // Phase 18: the selection-driven panels. Runs after the click-pick above so
    // they draw THIS frame's selection.
    void EditorApp::DrawSelectionPanels()
    {
        m_selection.Prune([reg = &m_runtime->Registry()](Astra::Entity e)
                          { return reg->IsValid(e); });
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Outliner))
            Arcane::Editor::DrawOutlinerPanel(m_runtime->Registry(), m_selection,
                                              *m_undo, m_editBinding, m_outliner,
                                              m_scene.SavedStateId(),
                                              m_panelVis.OpenFlag(Arcane::Editor::PanelId::Outliner));
        if (m_panelVis.IsVisible(Arcane::Editor::PanelId::Inspector))
            Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo,
                                               m_editBinding, m_runtime->CurrentProject(),
                                               m_inspector, &m_inspectorServices,
                                               m_panelVis.OpenFlag(Arcane::Editor::PanelId::Inspector));

        // (The hosted plugin's DrawUI now renders into its OWN ImGui context,
        // composited into the viewport texture above -- not the editor context.)
    }

    // ===================== THE CHROME FRAME (NRI Phase 3, Task 10) =========
    // Phase 19's GRAPH arm, and the moment the editor's main window starts
    // showing something on a --nri-graph run. Task 8 left this arm discarding
    // the ImGui frame and presenting nothing (an honest mid-phase state, stated
    // as one); this is the replacement it named.
    //
    // ---- THE FRAME'S DECLARED SHAPE, and why it is what it is -------------
    // Exactly three nodes: batch2d -> tonemap -> imgui -> present.
    //
    //   batch2d   with NO batcher. AddBatch2DNode drains nothing, and
    //             Batch2DNode::Record's first statement is the canvas CLEAR --
    //             so a null batcher IS the clear, not a degenerate draw ("an
    //             empty frame is a cleared canvas, not an error").
    //   tonemap   is the ONLY thing in the tree that produces a backbuffer
    //             handle at all (AddTonemapNode -> ImportSwapChainTexture), so
    //             it is not decoration on top of the clear -- it is how the
    //             clear reaches the surface, and how a frame acquires one.
    //             That is why "clear + imgui + present" has three nodes rather
    //             than two, and why there is no shorter honest spelling of it
    //             through this machinery.
    //   imgui     draws the EDITOR context's draw data onto that backbuffer,
    //             through the chrome context's ImGuiNriNode (NodeSet::hostHud,
    //             set by NriGraphContext::Create because a host-window context
    //             presents chrome by definition).
    //
    // AND NOTHING ELSE, which is a claim about four fields left DEFAULT:
    //   * `post`        -- the scene's post chain belongs to the VIEWPORT
    //                      frame (phase 10). Chrome is not graded.
    //   * `pickOutline` -- likewise the viewport's; the chrome context was
    //                      built with neither node (CreateGraphVehicles asks
    //                      for them on the offscreen context only), so the
    //                      declaration would be refused even if asked for.
    //   * `gameUi`      -- the plugin HUD lives INSIDE the viewport texture,
    //                      never over the editor chrome. Same node gate.
    //   * `capture`     -- and this one is a RULE, not just an absence. The
    //                      editor's screenshot/golden semantics capture the
    //                      VIEWPORT (WriteAutoScreenshot reads the offscreen
    //                      output; the editor golden harness is Task 13's), so
    //                      chrome pixels must never reach a capture path. A
    //                      capture node declared HERE would copy the chromed
    //                      backbuffer and quietly redefine what an editor
    //                      golden contains.
    //
    // ===== AND NOT ONE OF THOSE FOUR IS PINNED BY A TEST. ==================
    // ArcaneTests does not compile this TU at all (EditorAppFrame.cpp is not in
    // that project -- see this file's header comment), so nothing in the gate
    // can observe the FrameDesc built below. The [nri] case named "an
    // imgui-only shape (the editor's chrome)..." drives DeclareGraphFrame with
    // a shape it TRANSCRIBES by hand; it protects the ENGINE against declaring
    // nodes a shape did not ask for, and it would stay green if the line below
    // grew `frame.capture = true`. So the four absences above are held by this
    // comment and by review, and nothing else. Anyone adding a field here is
    // the only check there is.
    //
    // NRI Phase 3, Task 13 LANDED the golden harness this comment used to
    // point at as future work, and the honest update is that it does NOT add
    // an automated check that can see THIS function either -- same TU, same
    // "not compiled into ArcaneTests" wall. What it DOES give the `capture`
    // rule is a structural, greppably-provable guarantee one level up: every
    // golden-capture code path (EditorApp::CaptureEditorGolden, the NVRHI
    // arm; RenderSceneToViewport's capture block, the graph arm) reads pixels
    // ONLY from `m_viewportTargets` (the canvas texture / the offscreen
    // graph's ReadCapture) -- neither one names `m_graphChrome`,
    // `PresentChromeFrame`, or the swapchain backbuffer anywhere
    // (`grep -n "m_graphChrome\|PresentChromeFrame" EditorApp*.cpp` inside
    // either capture function returns nothing). So a chrome pixel reaching an
    // editor golden would require capture code that does not exist yet, not a
    // gate this function could silently defeat -- which is a materially
    // stronger position than "held by review alone", even though it is still
    // not a runnable assertion. D3c's self-compare (maxDelta 0 against its own
    // just-captured baseline) is the closest thing to a live proof this
    // constraint gets, and it is desk-only by nature (it needs a window and a
    // device).
    //
    // ---- WHERE THE DRAW DATA COMES FROM ----------------------------------
    // The ImGui frame phases 13-18 already built. RenderToDrawData() is
    // ImGui::Render() plus the layer's SetCurrentContext pin -- the same call
    // RuntimeFrame::RenderGraph makes, and the same reason it goes through the
    // layer rather than calling ImGui::Render() bare: a document, the game
    // context or a plugin may have left another context current, and the pin is
    // what makes this the EDITOR's draw data rather than whoever's was last.
    // The returned data lives until that context's next NewFrame -- a whole
    // frame away -- which is what lets the node copy the geometry at RECORD
    // time (NriGraphContext::FrameDesc::imgui).
    //
    // ---- WHAT IS INTERACTIVE ---------------------------------------------
    // This chrome IS the editor UI, so it is interactive by definition -- and
    // as of D3 exit (2026-08-18) it actually is again. Task 6's stance withheld
    // the SDL event tap on the graph flavor for the RUNTIME's HUD (an
    // accidental drag there would move the HUD in the shared imgui.ini and
    // rewrite the very `full` baseline checkpoint D3b compares against), but
    // the tap is this context's ONLY source of mouse BUTTON events, so the
    // collateral was total: the graph-mode editor could not be clicked AT ALL
    // -- menus, panels, viewport pick, gizmo. It was never a property of this
    // frame, and the first human click of the D3 drive checklist is what
    // surfaced it. D3b closed with both golden sets frozen, so the tap install
    // became unconditional (ImGuiLayer.cpp); NRI Phase 5a, Task 5 later folded
    // the whole withheld-tap flavor away entirely, so there is only one Init()
    // left and it always installs the tap.
    //
    // ---- THE HEARTBEAT ---------------------------------------------------
    // Published by NriGraphContext::RenderFrame itself, after the present and
    // on Presented frames only (NriDiagnostics::PublishHeartbeat with the
    // swapchain's COMPLETED fence value -- Task 5). It used to be the graph
    // path's 1:1 replacement for the NVRHI arm's m_gpu->FrameProgress().
    // EndFrame(); that arm (and FrameProgress() itself) is gone as of NRI
    // Phase 5a, Task 6, so this is simply the only heartbeat left, and it
    // needs no line here: this call reaches it.
    bool EditorApp::PresentChromeFrame()
    {
        // FIRST, and unconditionally: the ImGui frame DrawEditorUi began owes
        // exactly one Render / RenderToDrawData / EndFrameDiscard, and every
        // path out of this function must have discharged it. Doing it here --
        // ahead of the null check and both non-Presented outcomes below --
        // is what makes that true by structure rather than by three copies.
        ImDrawData* const chrome = m_gpu->Imgui().RenderToDrawData();

        // Defensive: CreateGraphVehicles refuses to continue past a chrome
        // context that could not be created (Main() returns 1 without ever
        // entering MainLoop), so this is unreachable rather than a state a run
        // can sit in. TRUE, not false: a frame that cannot present is not a
        // reason to stop counting frames and stop polling the plugin, which is
        // what a false return means to MainLoop.
        if (!m_graphChrome)
            return true;

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = chrome;
        // stage stays Full -- the ONE stage that declares a HUD node at all
        // (DeclareGraphFrame gates the host HUD on it). The editor's golden
        // harness and its stage semantics are Task 13's; Full is what an
        // ordinary run means on both hosts.
        const Arcane::NriGraphContext::FrameOutcome outcome =
            m_graphChrome->RenderFrame(frame);

        if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)
        {
            // Already latched (the "nri-graph" seam bumped RenderErrorCount),
            // so the exit code would be 2 on its own -- but 1 says WHERE the
            // run died and outranks it. Stops rather than spinning on a broken
            // device, at the top of the NEXT frame: returning false here means
            // MainLoop skips EndFrame and loops, and PumpFrameEvents reads
            // m_requestExit before any phase runs.
            NoteGraphFrameFailure("the chrome frame could not be recorded or presented");
            return false;
        }
        if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped)
        {
            // Routine: a zero-sized surface or an OUT_OF_DATE acquire, both
            // self-healing on the next resize event. FALSE -- no frame was
            // presented, so nothing downstream should count one. The sleep
            // matches RuntimeFrame::RenderGraph's: without a presented frame
            // there is no pacing wait, and a zero-sized window would
            // otherwise spin.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        }
        return true;
    }

    // Phase 19: present.
    //
    // NVRHI ARM DELETED, NOT PORTED (NRI Phase 5a, Task 6). This function
    // used to be `if (GraphMode()) return PresentChromeFrame(); <NVRHI
    // backbuffer acquire, command-list clear, ImGuiLayer::Render (deleted at
    // Task 5), submit, present, FrameProgress().EndFrame()>` -- already
    // unreachable (that predicate was unconditionally true from Phase 5a Task
    // 2b, and Task 11a has since removed it). Unlike CompositeGameUi/
    // RenderSelectionOutline just above, this function still does real work --
    // it forwards to PresentChromeFrame -- so it stays. GpuContext
    // has built no Swapchain, command list or GpuFrameProgress since Task 6
    // deleted Swap()/Cmd()/FrameProgress() along with the rest of its NVRHI
    // half, so the tail no longer compiles at all; there is nothing left to
    // guard, so the guard is gone too. (GpuFrameProgress the CLASS is gone as
    // well as of Task 9.5a -- it never had a second builder.)
    bool EditorApp::PresentFrame()
    {
        return PresentChromeFrame();
    }

    // Phase 20: end of frame -- plugin hot-reload poll + the --frames N budget.
    //
    // ===== THE DLL SWAP'S POSITION IS THE SAFETY, ON BOTH ARMS ==============
    // (NRI Phase 3, Task 12 -- pinned rather than reworked, because the shape
    // was already right and only its guarantee was unwritten.)
    //
    // PluginHost::Poll is where a rebuilt module is actually swapped in: it
    // notices the fresh mtime, waits out its own 250 ms debounce, and then
    // runs Reload -> plugin Shutdown, registry reset, FreeLibrary, LoadLibrary,
    // Init. THIS LINE is the only place in the editor that reaches it.
    //
    // It sits AFTER every recorder in the frame, which is what makes the swap
    // unable to race one:
    //   * phase 10 declared, compiled, recorded and SUBMITTED the viewport
    //     graph frame (RenderSceneToViewport -> RenderFrameOffscreen);
    //   * phase 19 did the same for the chrome frame and presented it
    //     (PresentChromeFrame / PresentFrame).
    // So at this statement nothing is recorded-but-unsubmitted, on either arm.
    //
    // AND WHAT THE GPU MAY STILL BE READING IS NEVER THE PLUGIN'S. Everything
    // a graph frame reads was copied into HOST-OWNED objects at record time or
    // earlier: the plugin's sprites go through Batcher2D -> the upload ring,
    // its HUD through m_gameImgui (a host ImGui context, a host atlas, a host
    // ImGuiNri) whose vertices ImGuiNriNode::Record copies into that same
    // ring, and its textures resolve Guid -> Assets pixels -> NriTextureCache.
    // The plugin owns no nri object, no descriptor and no command buffer, so
    // unmapping its image cannot invalidate anything in flight. That is a
    // property of the boundary (PluginABI's C vtable hands across no GPU
    // handle on this arm), not of this ordering -- but the ordering is what
    // makes it hold even for the ImDrawData, which lives in the game context
    // until the next NewFrame and is consumed entirely inside phase 10.
    //
    // MOVING THIS CALL EARLIER IN THE FRAME WOULD REOPEN THE QUESTION. Between
    // phase 10 and phase 19 the chrome frame has not been recorded yet while
    // the viewport frame is already submitted; a reload there would run the
    // registry reset between the two recorders, i.e. between a picture and the
    // ImGui pass that samples it. The other half of the build flow (the
    // re-engage + scene reload that PollModuleBuild performs when NO host is
    // watching) is at the frame TOP for the same family of reason.
    // NRI Phase 3, Task 13: the NVRHI arm's golden capture, called from
    // MainLoop right after RenderSelectionOutline and before
    // PumpEditorDocuments/DrawEditorUi. Mirrored RuntimeFrame::CaptureTail's
    // NVRHI half (ReadTexturePixels off the presented target ->
    // Arcane::GoldenArtifact), reading the OffscreenCanvas output after its
    // last composite -- the editor's viewport is a panel texture rather than
    // the window's own backbuffer.
    //
    // NRI Phase 5a, Task 4 deleted that whole arm (OffscreenCanvas is gone),
    // leaving only an `if (GraphMode())` no-op this function had already
    // reduced to on every real run: the graph arm's capture is armed as a
    // NODE inside RenderSceneToViewport's own FrameDesc instead (vp.capture),
    // and read back synchronously right there -- there is no separate "read
    // this texture" entry point on that recorder for a later phase to use,
    // which is exactly the reasoning CaptureGraphViewportPng's own comment
    // gives for why an auto-screenshot re-renders rather than reads the last
    // frame. Task 11a removed the predicate, which leaves this function
    // EMPTY. It and its one caller are inert scaffolding; retiring them is
    // outside a predicate collapse and is carried in that task's report.
    void EditorApp::CaptureEditorGolden()
    {
    }

    void EditorApp::EndFrame(LoopState& ls)
    {
        if (m_plugin) m_plugin->Poll();

        ++m_frameCount;
        if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) ls.running = false;
    }
}
