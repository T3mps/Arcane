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

#include <Arcane/Assets/Assets.hpp>      // Arcane::WritePngRgba (RenderSceneToViewport's capture block)
#include <Arcane/Assets/ImageCompare.hpp>   // --compare (Task 9): PixelData/ImageCompareOptions/CompareImages; also pulls in ImageIo.hpp's LoadPngRgba
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat -- the hang watchdog's liveness signal
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Host/ReferenceImages.hpp>   // --compare/--bless (Task 9): ResolveReference (MainLoop's pre-loop fail-fast)
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved -- the device-loss latch
#include <Arcane/Render/ShaderCompiler.hpp>   // --settle N's IsIdle() quiescence check (Task 9, mirrors RuntimeFrame.cpp)
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (gizmo drag target)
#include <Arcane/Scene/MeshSubmissionSystem.hpp>   // CollectMeshInstances (ArmGraphViewportFrame's opaque 3D pass, F2a Task 10)
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (Play view + camera rect); Arcane::ActivePerspectiveSceneCamera (the mesh pass's camera)
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
        // --compare / --bless (Task 9). Own private copy, matching
        // RuntimeApp.cpp's (which explains the reasoning in full): this is a
        // FILE PATH COMPONENT ("dx12"/"vulkan", matching HostConfig.cpp's
        // --backend Choices() and the shipped ReferenceProject fixtures
        // byte-for-byte), never Arcane::ToString(backend) ("D3D12"/"Vulkan",
        // the human-facing log/report label used elsewhere in this file).
        // Not shared with RuntimeApp.cpp's copy because the two live in
        // different DLLs/exes with no common host-tier TU to hold it -- see
        // this task's own dispatch for why a third copy (EditorApp.cpp's
        // ShutdownGraphPath also needs one) was the pragmatic choice over
        // inventing shared plumbing for four lines.
        const char* CompareBackendName(Arcane::GraphicsBackend backend)
        {
            return backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12";
        }

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

        // Boot is over; anything the watchdog reports from here on belongs to
        // the frame loop, not to a stale boot stage.
        Arcane::Diagnostics::SetPhase("editor frame loop");

        // ===== --compare / --bless (Task 9) =================================
        // Resolve the reference BEFORE the settle loop starts, mirroring
        // RuntimeApp::MainLoop's own pre-loop block (Finding 3 of the Task 8
        // dispatch audit) EXACTLY: a reference that does not exist will not
        // start existing after N wasted settle attempts, so a missing/
        // undecodable reference fails FAST here, at ZERO frames rendered,
        // rather than entering the loop with an invalid m_referencePixels
        // and spending the whole --settle budget reporting "could not
        // compare" on every attempt. CreateGraphVehicles has already
        // succeeded by the time Main() calls this function, so
        // ShutdownGraphPath's own `if (!ChromeGraph() && ...) return;` guard
        // does not swallow the report this fail-fast path still owes -- see
        // that function for the bless/settle/report folding this run's exit
        // (4, "compare-missing-reference") funnels through.
        if (!m_config.compareReference.empty())
        {
            const std::filesystem::path projectRoot =
                (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                            : std::filesystem::path{};
            const char* const backendName = CompareBackendName(m_config.backend);
            m_compareResolution = Arcane::ResolveReference(projectRoot, m_config.compareReference,
                                                            backendName);

            if (m_compareResolution.level == Arcane::ReferenceLevel::None)
            {
                if (!m_config.bless)
                {
                    ARC_ERROR("--compare '{}': no reference image on disk for backend '{}' -- "
                              "re-run with --bless to create one",
                              m_config.compareReference, backendName);
                    m_compareMissingFatal = true;
                    m_graphExit = 4;
                    ShutdownGraphPath();
                    return;
                }
                // else: --bless is present -- the normal FIRST bless. Fall
                // through with m_compareRequested false below (Finding 4 of
                // the Task 8 audit): there is nothing yet to compare
                // against, and the conjunct would be disabled for the whole
                // run regardless.
            }
            else if (!m_config.bless)
            {
                // A real reference exists and this run is not overwriting
                // it -- load it for the settle loop's compare conjunct.
                if (!Arcane::LoadPngRgba(m_compareResolution.path, m_referencePixels.width,
                                          m_referencePixels.height, m_referencePixels.rgba) ||
                    !m_referencePixels.Valid())
                {
                    ARC_ERROR("--compare '{}': reference '{}' exists but could not be decoded -- "
                              "treating it as missing (re-run with --bless to replace it)",
                              m_config.compareReference, m_compareResolution.path.string());
                    m_compareMissingFatal = true;
                    m_graphExit = 4;
                    ShutdownGraphPath();
                    return;
                }
            }
            // else: a reference exists but --bless will overwrite it -- also
            // nothing to load; m_compareRequested stays false either way.
        }

        // Finding 4 (Task 8 audit): disabled outright whenever --bless is
        // set, regardless of whether m_compareResolution actually found
        // anything -- see the member's own comment (EditorApp.hpp) for why
        // blessing must not also gate convergence.
        m_compareRequested = !m_config.compareReference.empty() && !m_config.bless;

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
            // path faults inside the driver rather than returning an error
            // code. Mirrors RuntimeApp::MainLoop.
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
            // PHASES 11, 12 AND THE CAPTURE ARE DISCHARGED BY PHASE 10.
            // RenderSceneToViewport declares ONE graph frame whose nodes cover
            // all three: ArmGraphViewportFrame fills FrameDesc::pickOutline/
            // pickables/selectedIds for 12 and BeginGameUiFrame's draw data
            // for 11, FrameDesc::capture carries the screenshot readback, and
            // phase 10 compiles and executes the lot.
            //
            // THE ORDERING CONTRACT is what the engine-side node comments
            // cite: the HUD composites after the scene render and before the
            // outline. Node order inside ONE frame enforces it, which is
            // strictly stronger than three separate statements here would be
            // -- see ArmGraphViewportFrame.
            //
            // The numbering keeps its gaps deliberately: phases 11 and 12 are
            // real slots in this frame's contract, they are simply satisfied
            // upstream, and renumbering would invalidate every engine-side
            // comment that reasons about "phase 11 then phase 12" order.
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
        // ONE resize, to this run's one presentation surface, and strictly
        // BETWEEN frames -- which the top of the loop structurally is
        // (RuntimeFrame::PumpAndResize carries the same pair for the same
        // reason). The CHROME context's swapchain is the surface bound to
        // this window. The VIEWPORT's offscreen output is NOT resized here at
        // all -- it tracks the panel, not the window, and phase 8 owns it.
        //
        // NOT UNDER --headless: there is no swapchain to resize, the window is
        // never mapped so a real resize event cannot arrive, and
        // NriGraphContext::Resize is HOST-WINDOW MODE ONLY (ResizeOffscreen is
        // the offscreen twin, and it is NOT wanted here either -- the offscreen
        // chrome target is deliberately fixed at the boot extent for the whole
        // run, which is what makes two runs' captures the same size).
        if (events.resized)
        {
            if (ChromeGraph() && !ChromeGraph()->IsOffscreen())
                ChromeGraph()->Resize(events.width, events.height);
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
        const double frameDt = wallDt;
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
        // the viewport texture by the graph's GameUi ImGui node -- phase 11)
        // sits over the
        // scene, so a click on one of its widgets must claim the pointer ahead
        // of both the plugin's gameplay input and the editor gizmo/click-pick.
        // Uses the game context's LAST-frame WantCaptureMouse: this frame's
        // game ImGui pass runs later (after this input phase closes -- see
        // BeginGameUiFrame), so its capture state is not known yet this frame. That
        // 1-frame lag matches the one already inherent to inViewport/
        // m_viewportActive (both computed from the previous frame's panel
        // hover/focus).
        //
        // UNCONDITIONAL: the viewport builds a real OffscreenImGuiLayer and
        // composites its draw data through a graph node, so its
        // WantCaptureMouse is a real answer rather than a stand-in.
        fs.gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
            InPlayMode(), inViewport, m_gameImgui->WantCaptureMouse());

        // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
        // game ImGui pass, which composites into the viewport AFTER this input
        // phase returns (see BeginGameUiFrame). Only the few values the game
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
                                // REFUSE a drag the 2D gizmo cannot express
                                // (final-review finding 4, F1). The write-back
                                // below reads the entity's WORLD pose through
                                // DecomposeTRS and demotes the dragged result
                                // through inverse(ParentWorldMatrix), and
                                // DecomposeTRS takes scale from the 2D
                                // PROJECTION of the basis columns -- which
                                // equals the true axis length only while those
                                // axes lie in the XY plane. Every other
                                // component of the pose is wrong off-plane too:
                                // `w` already lost the entity's world z, so even
                                // the translation is not world-exact.
                                //
                                // BOTH matrices, not just the parent's, and
                                // neither implies the other (all three cases
                                // pinned in EntityOpsTest.cpp):
                                //   * a tilted PARENT shortens the demoted local
                                //     basis, so a PURE TRANSLATE drag silently
                                //     rescales the child -- a 45 degree pitch
                                //     halves it, the projection being applied
                                //     once on the way in and once on the way
                                //     back out;
                                //   * a tilted ENTITY under a planar parent
                                //     reads short on the way IN and has its tilt
                                //     replaced outright by RotationAboutZ below,
                                //     so a translate drag flattens an
                                //     orientation it never touched;
                                //   * a tilted entity under an oppositely-tilted
                                //     parent has a PLANAR world basis and still
                                //     demotes through a non-planar inverse.
                                // Refusing on the parent alone (as F1 shipped)
                                // meant the gizmo warned in one of two visually
                                // identical situations and silently corrupted
                                // the other, which teaches a false lesson about
                                // when it can be trusted. Uniform restriction
                                // beats inconsistent safety; the cost is that
                                // translate is blocked on a tilted entity, and
                                // that is accepted -- you cannot meaningfully
                                // author a tilted pose with a 2D gizmo anyway.
                                //
                                // Before F1 none of this was reachable -- a float
                                // `rotation` cannot express a tilt -- and the
                                // widening made it authorable through the new
                                // Quat Inspector row while leaving the planar
                                // assumption in place. The honest answer is to
                                // decline, not to repair: repairing means
                                // carrying z, tilt and z-scale through the whole
                                // pipeline, which IS the 3D gizmo, which is F4.
                                //
                                // Declined per TARGET, not per drag, so a
                                // multi-selection still moves the members that
                                // ARE planar; skipping before SnapshotComponent
                                // keeps the refusal out of the undo step too.
                                // Checked once here rather than per frame in the
                                // write-back so the warning fires once per drag
                                // attempt, and because neither matrix can change
                                // mid-drag.
                                const glm::mat4 parentMat =
                                    Arcane::Edit::ParentWorldMatrix(*regPtr, e);
                                const glm::mat4 worldMat =
                                    Arcane::Edit::WorldMatrix(*regPtr, e);
                                const bool planarParent = Arcane::IsPlanarBasis(parentMat);
                                if (!planarParent || !Arcane::IsPlanarBasis(worldMat))
                                {
                                    const Arcane::Identity* id =
                                        regPtr->GetComponent<Arcane::Identity>(e);
                                    // Which of the two failed is the difference
                                    // between "fix the parent" and "fix this
                                    // entity", so the message names it rather
                                    // than covering both vaguely.
                                    const char* what = planarParent
                                        ? "has a basis that leaves the XY plane"
                                        : "sits under a parent whose basis leaves the XY plane";
                                    ARC_WARN("gizmo: \"{}\" (id {}) {} -- the 2D gizmo cannot "
                                             "move it without corrupting scale and orientation, "
                                             "so this drag leaves it alone (a 3D gizmo is F4)",
                                             id ? id->name : std::string("<unnamed>"), e.GetID(),
                                             what);
                                    continue;
                                }
                                m_undo->SnapshotComponent(e, ed);
                                // Stored WORLD pose (see gt above) -- the group
                                // delta below composes/replays in world space.
                                // Reuses the matrix the guard already walked.
                                m_gizmoDrag.targets.push_back({ e, Arcane::DecomposeTRS(worldMat) });
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
                    const glm::mat4 localMat =
                        glm::inverse(Arcane::Edit::ParentWorldMatrix(*regPtr, e)) * Arcane::ComposeTRS(w);
                    const Arcane::GizmoTransform r = Arcane::DecomposeTRS(localMat);
                    // Task 3 (F1): Transform is 3D but THE GIZMO IS STILL 2D
                    // (F4 makes it 3D), so only the components it actually has
                    // a handle for are written. position.z and scale.z are
                    // carried through UNCHANGED rather than being zeroed by a
                    // decomposition that never looked at them -- a translate
                    // drag must not silently flatten an entity's depth.
                    //
                    // rotation is the exception, and it WOULD be a real loss:
                    // RotationAboutZ replaces the whole quaternion, so any
                    // out-of-plane orientation on a dragged entity is discarded
                    // outright rather than preserved through the drag.
                    // Preserving it would mean a swing-twist split of the
                    // authored quaternion, which is 3D-gizmo work and belongs
                    // with the task that gives the user 3D handles.
                    //
                    // What keeps it from being a loss at all: the drag-start
                    // guard (see the targets loop) refuses any `e` whose WORLD
                    // basis leaves the XY plane, so every entity reaching this
                    // line has a rotation RotationAboutZ can express exactly.
                    // The same guard's parent arm is what makes the demotion
                    // above sound -- inverse(ParentWorldMatrix) is only a planar
                    // matrix while the parent is one, and without that check a
                    // tilted parent turned the projection in DecomposeTRS into
                    // silent scale corruption on a drag that touched no scale
                    // handle at all (final-review finding 4). Do not relax
                    // either arm without making this line 3D-exact first.
                    et->position = glm::vec3(r.position, et->position.z);
                    et->rotation = Arcane::RotationAboutZ(r.rotation);
                    et->scale    = glm::vec3(r.scale, et->scale.z);
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
        m_runtime->Loop().Advance(simDt,
            [&](double dt)          { if (m_plugin) m_plugin->FixedUpdateAll(dt); },
            [&](double dt, double a){ if (m_plugin) m_plugin->UpdateAll(dt, a); });
        m_runtime->AudioSystem().Update(simDt);
    }

    // Phase 8: deferred viewport resize. Must stay before the scene render.
    void EditorApp::ApplyPendingViewportResize()
    {
        m_viewportTargets.ApplyPendingResize(ChromeGraph());
    }

    // Deferred resize: the Viewport panel's content-region size measured LAST
    // frame, applied at the START of THIS frame (before this frame records).
    // This mirrors the m_viewportRect/m_viewportActive one-frame lag on
    // EditorApp -- it avoids a same-frame use-after-free where a resize
    // (called right after ImGui::Image bakes the current texture pointer into
    // this frame's draw list) synchronously frees that very texture before
    // ImGui replays the draw list at Render time: ResizeOffscreen destroys the
    // output texture synchronously.
    void EditorApp::ViewportTargets::ApplyPendingResize(Arcane::NriGraphContext* chrome)
    {
        // ================= THE RESIZE CONTRACT ============================
        // Its shape is prescribed, clause for
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
        //
        // A NULL `graph` means one thing only: a project switch whose rebuild
        // failed (SwitchProject's "render_bridge"), already exiting. This
        // frame still has to finish, and it finishes by doing nothing here.
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
            // THE DEFERRED FIRST-OPEN FRAMING. HERE and not in phase 6c
            // (UpdateEditorCamera) for two reasons: this is after
            // ApplyPendingViewportResize, so the fit sees THIS frame's
            // viewport extent, and it is before the SetCamera push below --
            // the one every render path reads.
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

        // THE POST CHAIN'S HANDOFF IS NOT MADE HERE, and needs no statement:
        // the resolver runs device-less (StageSpriteTables) and PostChainCache
        // publishes BYTES, which travel to the recorder as FrameDesc::post +
        // FrameDesc::globals and are read fresh at the render site in the next
        // phase (RenderSceneToViewport).
    }

    // Phase 10: the scene render. Must run BEFORE the ImGui pass builds the
    // Viewport panel's Image, and the gizmo inside it is drawn AFTER the scene
    // submit so it renders on top.
    // ===== "IS A CAPTURE WANTED AT ALL", IN ONE PLACE =======================
    // Named rather than spelled out at each of the two capture sites below,
    // because Task 8 was bitten on the RUNTIME by exactly the divergence a
    // second copy invites: that host's gate read `screenshotPath` ALONE, so
    // `--report` with pixel probes and no `--screenshot` ran, exited 0, wrote a
    // well-formed JSON, and reported "no capture set" for every probe -- a
    // silent success an agent reads as a pass. RuntimeFrame.cpp arms on
    // capture-WANTED (`!screenshotPath.empty() || !reportPath.empty()`).
    //
    // TASK 9: THIS HOST'S GATE IS NOW THE SAME TWO-CLAUSE ONE, no longer the
    // narrower `--screenshot` alone this comment used to defend. --report is
    // wired (PresentChromeFrame's settle branch, ShutdownGraphPath's
    // VerifyReport), so a bare `--report r.json` with no `--screenshot` must
    // still arm the chrome readback -- without the OR, that combination would
    // arm nothing, and the report's capture section would silently read "no
    // capture set" even though the run converged cleanly. `--probe` is
    // deliberately NOT a third clause here: it stays refused at main.cpp, and
    // this host evaluates no probes, so widening this predicate for it would
    // arm a readback nothing downstream reads -- see main.cpp's own refusal
    // comment. Wire --probe here and both places must grow together the
    // same day.
    static bool CaptureWanted(const Arcane::HostConfig& cfg) noexcept
    {
        return !cfg.screenshotPath.empty() || !cfg.reportPath.empty();
    }

    void EditorApp::RenderSceneToViewport()
    {
        // ================= THE VIEWPORT FRAME =============================
        // THIS PHASE DECLARES THE WHOLE VIEWPORT FRAME -- the scene, the game
        // HUD (phase 11's content) and the pick + outline chain (phases 12 and
        // 17's), because all three are NODES in ONE declaration rather than
        // three passes submitted at three points in the frame.
        // ArmGraphViewportFrame below is the other phases' half; see its
        // comment for why it has to run HERE and not where those phases sit.
        //
        // WHAT THIS FRAME DOES NOT CARRY, so the gap stays named rather than
        // discovered:
        //   * imgui        -- HOST CHROME's draw data. Not the game HUD (that
        //                     is `gameUi`): this viewport has no chrome on it,
        //                     and the editor's own chrome belongs to phase 10
        //                     on the CHROME context. Absent here.
        //   * capture      -- the auto-screenshot re-renders through
        //                     CaptureGraphViewportPng's OWN fresh frame rather
        //                     than reading this one. The --screenshot capture
        //                     piggybacks on THIS declaration instead, on the
        //                     run's last frame only -- see below.
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
            // canvas -> tonemap, which is also how a caller asks for the frame
            // WITHOUT a post chain at all.
            vp.post    = m_resolver ? m_resolver->PostDesc() : nullptr;
            vp.globals = &globals;
            // Phases 11, 12 and 17's contribution to THIS declaration. Strictly
            // AFTER SubmitSceneToBatcher above, which is not incidental: it
            // runs the plugin's DrawUI, and phase 11 follows phase 10. A
            // DrawUI that mutates the scene must land on THIS frame, not the
            // next.
            ArmGraphViewportFrame(vp);

            // THE --screenshot CAPTURE, armed on the LAST frame only -- the
            // same "+1" the runtime's RenderGraph uses (there io.frameCount is
            // bumped in CaptureTail, i.e. after that arm returns; here
            // EndFrame's own m_frameCount increment happens later still, at
            // phase 20). Piggybacking on the frame ALREADY being declared
            // here, rather than re-rendering the way CaptureGraphViewportPng
            // does for the auto-screenshot, is what keeps the captured picture
            // governed by exactly the frame the run was showing.
            //
            // `maxFrames != 0` matches the runtime: both capture on the LAST
            // frame, and without --frames there is no last frame.
            //
            // AND NOT UNDER --headless, which is the whole point of Task 11:
            // there the screenshot is the COMPOSITED EDITOR FRAME, captured off
            // the chrome context in PresentChromeFrame (phase 19) -- panels,
            // docking, asset browser and this viewport's texture inside its
            // panel. Leaving this arm live as well would have the viewport
            // capture land first and the chrome capture overwrite it with a
            // different picture at the same path: same run, same flag, two
            // meanings. The WINDOWED behaviour is unchanged -- there the chrome
            // context is a swapchain nothing reads back, so the viewport
            // texture remains the only editor screenshot there is (main.cpp's
            // flag table says so).
            const bool isCaptureLastFrame = m_config.maxFrames != 0 &&
                                            (m_frameCount + 1) >= m_config.maxFrames &&
                                            CaptureWanted(m_config) &&
                                            !m_config.headless;
            vp.capture = isCaptureLastFrame;

            const Arcane::NriGraphContext::FrameOutcome outcome =
                m_viewportTargets.graph->RenderFrameOffscreen(vp);
            // Skipped stays UNACTED-ON, and that is the routine case: a
            // collapsed panel, or a resize whose replacement could not be
            // created -- OffscreenTextureId() is 0 then and phase 16 draws no
            // Image, which IS the signal. A run whose last frame lands Skipped
            // simply does not capture this iteration: m_frameCount is driven
            // by phase 19/20 (the CHROME frame's own success), not by this
            // outcome.
            //
            // Failed is acted on. It has already bumped RenderErrorCount, so
            // the run's exit code
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

            // The readback, RIGHT HERE rather than a later phase:
            // ReadCapture is synchronous (it idles the device internally --
            // the same stall CaptureGraphViewportPng pays, and this call is
            // rare -- ordinarily exactly once, on the run's last frame), and
            // the captured pixels belong to the frame that was JUST
            // presented -- reading them any later risks the next frame's
            // Begin/Draw overwriting the vehicle's transient state first.
            //
            // NOT ALWAYS EXACTLY ONCE, though: if the CHROME frame (phase 19,
            // PresentFrame/PresentChromeFrame) fails to present this same
            // iteration, MainLoop `continue`s WITHOUT running EndFrame,
            // m_frameCount does not advance, and `isCaptureLastFrame` is
            // unchanged on the retry -- so a second viewport frame that DOES
            // present here re-attempts the capture and simply overwrites the
            // same file with equivalent content. Harmless, and orthogonal to
            // whatever made the chrome frame fail.
            if (isCaptureLastFrame && outcome == Arcane::NriGraphContext::FrameOutcome::Presented)
            {
                std::uint32_t cw = 0, ch = 0;
                std::vector<unsigned char> actual;
                if (!m_viewportTargets.graph->ReadCapture(cw, ch, actual))
                {
                    // A WARN, never an exit code -- the same contract
                    // RuntimeFrame::CaptureTail keeps.
                    // "(no capture landed)" -- the SAME wording
                    // RuntimeFrame::CaptureTail uses. It was
                    // "(viewport capture readback)" here, a third variant
                    // across the two hosts, which makes a log line unsearchable
                    // for the one failure it names.
                    ARC_WARN("screenshot FAILED: {} (no capture landed)",
                             m_config.screenshotPath);
                }
                else
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, cw, ch, actual.data()))
                        ARC_INFO("screenshot written: {}", m_config.screenshotPath);
                    else
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                }
            }
            return;
        }

        // A session that lost its viewport context to a failed post-switch
        // rebuild has nothing to fall through to: the exit is already
        // requested, and this frame just ends here without a fault.
    }

    // The scene submission plus the two Edit-mode overlays, against whichever
    // batcher the caller hands in. Kept as its own function so the ORDER of
    // these statements is stated in one place -- every overlay's position
    // relative to the scene submission is part of the contract.
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

        // GizmoLive() rather than m_gizmoEnabled: this draw goes into the
        // SCENE BATCH, so there is no downstream gate that could confine it --
        // the batcher's content IS the scene. Anything that must not appear in
        // the scene has to be excluded HERE.
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

    // Phase 11's CPU HALF -- see the declaration. The GPU composite is a NODE
    // inside the viewport frame rather than a call from here, so this is all
    // of phase 11 there is as code, and ArmGraphViewportFrame is its only
    // caller.
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

    // Phases 11, 12 and 17's contribution to the ONE declaration phase 10
    // makes.
    //
    // WHY IT IS CALLED FROM PHASE 10 RATHER THAN FROM THOSE PHASES. All three
    // are NODES in one graph that is declared, compiled and executed by a
    // single RenderFrameOffscreen call -- so everything the frame will contain
    // has to be known BEFORE that call, and phase 10 is where it is made.
    // Moving the call EARLIER is not an option either: the game HUD's draw
    // data must be produced AFTER the scene submission, so that a plugin whose
    // DrawUI mutates the scene lands on this frame rather than the next.
    // Phases 11, 12 and 17 keep their gates and each points at this
    // function.
    void EditorApp::ArmGraphViewportFrame(Arcane::NriGraphContext::FrameDesc& vp)
    {
        // ---- phase 11's content: the game HUD ---------------------------
        // Play-mode only, exactly as that phase is. The draw data lives in the
        // game context until its next NewFrame -- a whole frame away -- which
        // is what lets the node copy the geometry at record time
        // (FrameDesc::gameUi).
        if (BeginGameUiFrame())
        {
            // The draw data goes to the graph, which declares the GameUi
            // node for it. A caller that wants the frame WITHOUT the game HUD
            // leaves vp.gameUi null instead -- that is the whole mechanism
            // now, and it lives in FrameDesc rather than in a stage ordinal.
            vp.gameUi = m_gameImgui->RenderToDrawData();
        }

        // ---- F2a Task 10's content: the opaque 3D pass --------------------
        // UNCONDITIONAL, unlike the pick + outline chain below: a scene's
        // mesh instances are ordinary scene content, and SubmitSceneToBatcher
        // (the 2D equivalent, called from RenderSceneToViewport just before
        // this function) draws in Edit AND Play alike -- neither is gated on
        // InPlayMode(). This section has to sit ABOVE the pick chain's early
        // return just below, or a frame with no selection and no hover would
        // silently stop submitting meshes too.
        //
        // m_meshInstances is a MEMBER for the same "FrameDesc borrows this
        // past the statement that fills it" reason m_pickDrawables is (see
        // EditorApp.hpp). CollectMeshInstances is the TESTED sweep
        // (MeshSubmissionSystem.hpp); this call site stays thin on purpose,
        // since EditorAppFrame.cpp is not compiled into ArcaneTests.
        Arcane::CollectMeshInstances(m_runtime->Registry(), m_meshInstances);
        // vp.mesh is left at FrameDesc's own default (null) unless both an
        // instance and a camera exist below -- the same "ask for the frame
        // WITHOUT this stage by leaving the field null" mechanism vp.gameUi
        // just used above.
        if (!m_meshInstances.empty())
        {
            // THE GUARDED PATH ONLY -- MeshSceneDesc's own comment
            // (MeshNode.hpp) is explicit that the caller owes valid
            // matrices, and ActivePerspectiveSceneCamera is the one function
            // that either hands back a validated (view, projection) pair or
            // nullopt, never identity. No active perspective camera means
            // there is nothing to draw the pass WITH, so vp.mesh stays null
            // -- the same "leave it alone rather than invent a viewpoint"
            // contract ActiveSceneCamera documents for the 2D path
            // (SceneCamera.hpp).
            const float vw = (float)ViewportWidth();
            const float vh = (float)ViewportHeight();
            const float aspect = (vh > 0.0f) ? (vw / vh) : 1.0f;   // same zero-height guard SubmitSceneToBatcher's camera-rect overlay uses
            if (const auto cam = Arcane::ActivePerspectiveSceneCamera(m_runtime->Registry(), aspect))
            {
                m_meshScene.instances = m_meshInstances;
                m_meshScene.view       = cam->view;
                m_meshScene.projection = cam->projection;
                // NO SCENE LIGHT, DELIBERATELY: this engine has no light
                // component anywhere -- Scene/Components.hpp declares none
                // and SceneModule.hpp registers none -- so
                // lightDirection/lightColor/ambient are left at
                // MeshSceneDesc's own documented defaults (MeshNode.hpp:
                // 237-239). A light component is future work and out of
                // scope for F2a; inventing one here would land an
                // unreviewed scene-schema change in a host .cpp with zero
                // test coverage rather than in a reviewed, tested
                // component.
                vp.mesh = &m_meshScene;
            }
        }

        // ---- phases 12 + 17's content: the pick + outline chain ----------
        // THE HOVER GOES THROUGH HoverLive(), not m_gameUi.inViewport
        // directly: that field is derived from the LIVE pointer, so reading it
        // here would make what this frame outlines a function of where the
        // mouse happens to be. See the predicate's own comment.
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
        // NOTE THE ASYMMETRY WITH THE ENGINE: RgFrameShape::pickOutline is
        // deliberately independent of how the scene is sliced, because the
        // runtime arms it from `--pick-probe` and nothing else. The EDITOR's
        // own decision about when to arm the chain therefore lives HERE, at
        // the call site, rather than in DeclareGraphFrame -- gating it in the
        // engine would change what that flag means on the other host.
        if (!wantOutline && !wantPick)
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
        // entry IS hit-proxy id k+1.
        const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
        m_pickDrawables.clear();
        Arcane::CollectPickables(m_runtime->Registry(), view, m_pickDrawables);

        // Every selected entity that made it into THIS frame's id pass, over
        // the same drawables that were just handed to the pick node rather
        // than over a table some earlier pass left behind. Empty in Play,
        // which is how
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
        // in Play, where the outline must not light anything.
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
    // ===== THIS PHASE'S WORK HAPPENS IN PHASE 10 =====
    // The HUD is a NODE inside the viewport frame, and that frame is
    // declared, compiled and executed by phase 10's single
    // RenderFrameOffscreen call -- which has already happened by the time
    // control reaches here. ArmGraphViewportFrame (above) is where this
    // phase's content is produced, through the SAME BeginGameUiFrame helper,
    // at the same point relative to the scene submission.
    //
    // THE GAME HUD HAS ITS OWN ImGui CONTEXT, and that is load-bearing rather
    // than tidy: the editor's context has io.IniFilename pointed at the
    // PER-PROJECT LAYOUT FILE, so a plugin window drawn through it would
    // persist into the user's layout permanently -- the imgui.ini veto class.
    // The game context's io.IniFilename is null for exactly that reason.
    //
    // PHASE 12 (Edit-mode selection/hover outline) HAS NO FUNCTION OF ITS OWN.
    // It occupied the same slot as the Play-only game-HUD composite of phase 11
    // -- the two are mutually exclusive by mode -- and both had to run after the
    // scene render and before the editor ImGui pass. Both are satisfied inside
    // phase 10: the picker and the selection outline are NODES in the
    // viewport context's own frame
    // (PickNode/OutlineNode) armed through FrameDesc::pickOutline/pickables/
    // selectedIds/hoverPixel, and that frame is declared, compiled and executed
    // by phase 10's single RenderFrameOffscreen call. ArmGraphViewportFrame
    // (above) carries phase 12's gate VERBATIM -- Edit mode, and a selection
    // or a cursor in the viewport -- and maps m_selection through the k+1 id
    // assignment CollectPickables defines.

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
        // preview vehicles that LAST frame's closes handed over. By now the
        // chrome frame whose draw lists named them has
        // been recorded AND submitted, which is the whole condition the
        // deferral exists to satisfy.
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
        // A FLIP HERE ALSO REPLACES THE SCENE: both PlaySession::Play and
        // ::Stop go through Runtime::Snapshot/RestoreRegistry, which destroys
        // and replaces the registry OBJECT -- so a click-pick in flight names
        // entities of a registry that no longer exists. Noted here rather than
        // inside PlaySession because
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
        if (browserActions.createMesh)
        {
            // F2a, Task 9: the "+ Mesh" button. No dialog -- unlike a
            // material/instance, a mesh names no user-meaningful path up
            // front (MeshAssetData's own defaults already describe a
            // complete asset), so this mints straight into the project's
            // Content root and opens the result, same two-step shape as the
            // createSpriteFrom branch just above.
            if (const Arcane::Guid minted = MintMeshAsset(); minted.IsValid())
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
        // A null `graph` -- a project switch whose rebuild failed
        // (SwitchProject's "render_bridge"), which sets m_requestExit, read at
        // the TOP of the next frame so THIS frame still runs phase 16 --
        // answers 0 for the same reason, and 0 is the correct answer.
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
        // ============ THE PICK IS DEFERRED, NOT SYNCHRONOUS ============
        // The readback is a graph COPY node whose value lands
        // kSwapchainFramesInFlight frames later, so the click is ARMED here
        // and the resulting Select/Toggle/Clear is APPLIED here on a LATER
        // frame. That latency is inherent to reading a GPU-rasterised id back
        // without stalling the device, and it is the only pick path there is.
        //
        // TWO STEPS, AND THE ORDER IS DELIBERATE: apply what has landed FIRST,
        // then arm this frame's click. The other order would let a fresh click
        // discard an answer that had already arrived.
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
                        // deselect-all, and the ctrl state is the one
                        // captured AT THE CLICK rather than whatever is held
                        // now.
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

    // ===================== THE CHROME FRAME ===============================
    // Phase 19: what puts the editor's main window on screen.
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
    //                      editor's screenshot semantics capture the VIEWPORT
    //                      (WriteAutoScreenshot and RenderSceneToViewport's
    //                      capture block both read the OFFSCREEN output), so
    //                      chrome pixels must never reach a capture path. A
    //                      capture node declared HERE would copy the chromed
    //                      backbuffer and quietly redefine what an editor
    //                      screenshot contains.
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
    // WHAT THE `capture` RULE DOES HAVE is a structural, greppable guarantee
    // one level up: every capture code path reads pixels ONLY from
    // `m_viewportTargets`. That is a single path -- RenderSceneToViewport's
    // capture block, over the offscreen graph's ReadCapture -- and it does not
    // name `m_graphChrome`, `PresentChromeFrame`, or the swapchain backbuffer
    // anywhere (`grep -n "m_graphChrome\|PresentChromeFrame" EditorApp*.cpp`
    // inside either capture function returns nothing). So a chrome pixel
    // reaching an editor screenshot would require capture code that does not
    // exist, rather than a gate this function could silently defeat.
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
    // This chrome IS the editor UI, so it is interactive by definition. The
    // SDL event tap ImGuiLayer installs is this context's ONLY source of mouse
    // BUTTON events -- withholding it does not merely cost a drag, it makes
    // the editor unclickable outright: menus, panels, viewport pick, gizmo.
    // ImGuiLayer::Init installs it unconditionally for exactly that reason.
    //
    // ---- THE HEARTBEAT ---------------------------------------------------
    // Published by NriGraphContext::RenderFrame itself, after the present and
    // on Presented frames only (NriDiagnostics::PublishHeartbeat with the
    // swapchain's COMPLETED fence value). It is the only heartbeat there is,
    // and it needs no line here: this call reaches it.
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
        if (!ChromeGraph())
            return true;

        Arcane::NriGraphContext::FrameDesc frame;
        frame.imgui = chrome;

        // ===== THE --headless FORK, AND THE FULL-FRAME CAPTURE ===============
        // The absence table above says an editor screenshot has never
        // contained a chrome pixel: `frame.capture` was never set here, and
        // every capture path read the VIEWPORT's texture instead -- so the
        // Inspector, the asset browser and the docking chrome were invisible in
        // every editor PNG ever taken (main.cpp's flag table records it).
        //
        // Under --headless that is now false ON PURPOSE, and the table above
        // is amended by this block rather than contradicted by it: this context
        // has NO SWAPCHAIN, so "the backbuffer" it captures is the offscreen
        // colour target the whole composited editor was just drawn into --
        // chrome, panels, and the viewport texture sampled inside its panel.
        // That is the only picture that can answer "did the editor draw", which
        // is the entire job of the mode.
        //
        // THE MECHANISM NEEDED NO INVENTION. ImGuiNri::Init takes a device and
        // a pipeline cache, NOT a window, and this host sets only
        // ImGuiConfigFlags_DockingEnable with no ViewportsEnable -- so ImGui
        // never spawns an OS window for a floating panel and every pixel of the
        // editor is inside this one target by construction. The runtime already
        // composites its own ImGui HUD into an offscreen capture the same way.
        //
        // THE LAST BASE FRAME, AND EVERY SETTLE ATTEMPT AFTER IT. By the same
        // `+1` arithmetic RenderSceneToViewport uses and for the same reason:
        // phase 20's EndFrame is what increments m_frameCount, and it has not
        // run yet this iteration. `maxFrames != 0` matches both other hosts --
        // without --frames there is no last frame -- and --headless cannot be
        // passed without --frames anyway (HostConfig refuses it at parse
        // time).
        //
        // --settle N (Task 9): this predicate's NAME still says "the last
        // frame", and under an ordinary (non-settle) run it is exactly that --
        // the ordinary branch below stops the loop (via EndFrame) the moment
        // it fires. Under --settle it stays true for EVERY frame from here
        // on, because m_frameCount only grows (see EndFrame's own comment) --
        // which is exactly what is wanted: each such frame is one more settle
        // attempt, and the settle branch below -- not this predicate -- is
        // what decides when the loop actually ends (converged, or the
        // attempt budget spent). This is the SAME identity
        // RuntimeFrame::RenderGraph's willBeLastFrame states for the
        // runtime's own capture arm: "arm every frame past the base budget"
        // and "arm the one last frame" are one expression, not two.
        const bool offscreenChrome = ChromeGraph()->IsOffscreen();
        const bool pastBase = offscreenChrome &&
                              CaptureWanted(m_config) &&
                              m_config.maxFrames != 0 &&
                              (m_frameCount + 1) >= m_config.maxFrames;
        frame.capture = pastBase;

        // A non-null `imgui` is what declares the HUD node at all
        // (DeclareGraphFrame gates the host HUD on exactly that), and the
        // chrome frame always wants one.
        //
        // RenderFrameOffscreen vs RenderFrame is NOT a style choice: each
        // REFUSES on the other's mode (NriGraphContext.hpp), so the fork is
        // mandatory the moment this context can be either.
        const Arcane::NriGraphContext::FrameOutcome outcome =
            offscreenChrome ? ChromeGraph()->RenderFrameOffscreen(frame)
                            : ChromeGraph()->RenderFrame(frame);

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

        // THE READBACK, right here rather than in a later phase, for the reason
        // RenderSceneToViewport's own capture block states: ReadCapture is
        // synchronous (it idles the device internally) and the pixels belong to
        // the frame that was JUST submitted -- reading them any later risks the
        // next frame's Begin/Draw overwriting the vehicle's transient state
        // first. Reached only on Presented: both other outcomes returned above.
        //
        // DEFENSIVE (mirrors RuntimeFrame.cpp:710-723 exactly -- fix round 1,
        // item 5, there): HostConfig::Parse already refuses --settle without
        // --screenshot/--report at parse time, so `m_config.settleAttempts
        // != 0 && !CaptureWanted(m_config)` should be unreachable from any
        // real command line. But `pastBase` folds CaptureWanted in as one of
        // its conjuncts, so in that state `pastBase` can NEVER become true no
        // matter how many frames run -- `if (!pastBase) return true;` below
        // would then fire every single iteration forever, m_settleAttemptsUsed
        // would never advance (it lives past that early return), and none of
        // EndFrame's stop conditions -- m_settleConverged, or m_settleBail
        // leaving Keep -- could ever become true either, because BOTH are
        // written by the settle branch that early return skips. That
        // is an unstoppable run -- the exact hazard --headless's own
        // --frames-required refusal exists to prevent -- if some caller ever
        // builds a HostConfig bypassing Parse (a test, a future embedder).
        // Fail CLOSED rather than hang: raise m_settleAborted, which EndFrame
        // reads as an unconditional stop -- the same "stop now" effect
        // RuntimeFrame::CaptureTail's own `return true` (which directly
        // controls its while condition) gets for free there.
        //
        // THIS USED TO FORGE THE COUNTER (m_settleAttemptsUsed =
        // m_config.settleAttempts) so EndFrame's attempts-only check would
        // trip. That trick DIED with the bail conjunction: once the loop also
        // requires the TIME bound to be spent, saturating the attempt count
        // stops nothing at all, and the unstoppable run this branch exists to
        // prevent would come straight back. A dedicated flag also leaves
        // m_settleAttemptsUsed telling the truth about attempts actually taken.
        if (m_config.settleAttempts != 0 && !CaptureWanted(m_config))
        {
            ARC_ERROR("--settle has nothing to compare against (--screenshot and --report are "
                      "both empty) -- this should have been refused at parse time; stopping "
                      "rather than spinning forever");
            m_settleConverged = false;
            m_settleAborted   = true;
            return true;
        }

        if (!pastBase)
            return true;

        // ===== --settle N (Task 9) ==========================================
        // Everything from here down is RuntimeFrame::CaptureTail's settle
        // branch, ported rather than reinvented -- Step 1 of this task's own
        // brief. Two shapes:
        //   * settleAttempts == 0 -- the ORDINARY single-shot tail, unchanged
        //     from Task 8/11: one readback, one screenshot, done.
        //   * settleAttempts != 0 -- one more ATTEMPT: compare this readback
        //     against the PREVIOUS attempt's (byte-equal), conjoin the shader
        //     compiler's IsIdle() (a stability predicate is not the
        //     QUIESCENCE one this mode needs -- see RuntimeFrame.cpp's own
        //     comment on why), and -- when --compare is running -- conjoin a
        //     reference match. Converged: write the screenshot (if any) and
        //     stash the agreed capture. Not yet: stash THIS attempt as the
        //     new baseline and keep going. Budget spent without converging:
        //     stop with m_settleConverged left false and m_captureRead left
        //     false -- no screenshot, no report SetCapture, an honest
        //     absence rather than a frame this run never actually agreed on.
        // The loop's actual STOP decision is EndFrame's, reading
        // m_settleConverged / m_settleAttemptsUsed -- this function always
        // returns true from here down (a chrome frame WAS presented this
        // iteration), exactly like the ordinary tail always did.
        if (m_config.settleAttempts == 0)
        {
            std::uint32_t cw = 0, ch = 0;
            std::vector<unsigned char> pixels;
            if (!ChromeGraph()->ReadCapture(cw, ch, pixels))
            {
                // A WARN, never an exit code -- the same contract
                // RuntimeFrame::CaptureTail and the viewport capture keep, and
                // the same wording, so one grep finds every host's version.
                ARC_WARN("screenshot FAILED: {} (no capture landed)", m_config.screenshotPath);
            }
            else
            {
                if (!m_config.screenshotPath.empty())
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, cw, ch, pixels.data()))
                    {
                        ARC_INFO("screenshot written: {} ({}x{}, composited editor frame -- "
                                 "chrome, panels and viewport)", m_config.screenshotPath, cw, ch);
                    }
                    else
                    {
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                    }
                }

                // Stashed for VerifyReport regardless of --screenshot,
                // mirroring RuntimeFrame::CaptureTail's non-settle tail
                // exactly: a bare `--report` with no `--screenshot` still
                // needs these for SetCapture.
                m_captureWidth  = cw;
                m_captureHeight = ch;
                m_captureRgba   = std::move(pixels);
                m_captureRead   = true;
            }
            return true;
        }

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> actual;
        const bool read = ChromeGraph()->ReadCapture(w, h, actual);
        // Counted here, beside the readback -- NEVER off m_frameCount (see
        // EndFrame's own comment on why: a Skipped chrome present returns
        // false above and never reaches this line, so a counter tied to
        // frame advancement could spin forever on exactly that path, where
        // the runtime's cannot).
        ++m_settleAttemptsUsed;

        // Stamp the settle phase's own start on its FIRST attempt, and measure
        // from there. NOT from process start: that would charge boot, project
        // open and the whole --frames budget against the convergence budget,
        // making the timeout depend on how long the scene took to load.
        if (m_settleAttemptsUsed == 1)
            m_settleStartedAt = std::chrono::steady_clock::now();
        m_settleElapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_settleStartedAt).count());

        if (!read)
        {
            ARC_WARN("--settle attempt {}/{}: capture FAILED (no readback landed this frame)",
                     m_settleAttemptsUsed, m_config.settleAttempts);
        }
        else
        {
            const bool byteEqual = m_previousCaptureValid &&
                                    w == m_previousCaptureWidth && h == m_previousCaptureHeight &&
                                    actual == m_previousCaptureRgba;
            const bool idle = m_shaderCompiler ? m_shaderCompiler->IsIdle() : true;

            // PLAN B (Task 8, ported here by Task 9): the comparison is a
            // THIRD conjunct, evaluated HERE -- inside the loop -- and not
            // after it. m_compareRequested is false whenever --bless was
            // given (EditorApp.hpp's own comment), so `matches` stays its
            // default `true` on any run that never asked for --compare, or
            // that paired it with --bless -- this conjunct degrades to
            // Plan A's `byteEqual && idle` in both cases, exactly like the
            // runtime's.
            bool matches = true;
            if (byteEqual && idle && m_compareRequested)
            {
                Arcane::PixelData actualPixels;
                actualPixels.width  = w;
                actualPixels.height = h;
                actualPixels.rgba   = actual;
                Arcane::ImageCompareOptions compareOptions;
                compareOptions.maxDiffPixels     = m_config.maxDiffPixels;
                compareOptions.maxDiffPixelRatio = m_config.maxDiffPixelRatio;
                m_compareResult    = Arcane::CompareImages(m_referencePixels, actualPixels,
                                                            compareOptions);
                matches            = m_compareResult.passed;
                m_compareEvaluated = true;
            }

            if (byteEqual && idle && matches)
            {
                m_settleConverged = true;
                ARC_INFO("--settle: CONVERGED after {} attempt(s) -- frame {} matches frame {} "
                         "byte-for-byte ({}x{}), compiler idle", m_settleAttemptsUsed,
                         m_frameCount + 1, m_frameCount, w, h);

                // The exact pixels the composited editor showed -- same as
                // the ordinary tail above -- this IS the converged capture.
                if (!m_config.screenshotPath.empty())
                {
                    if (Arcane::WritePngRgba(m_config.screenshotPath, w, h, actual.data()))
                    {
                        ARC_INFO("screenshot written: {} ({}x{}, composited editor frame -- "
                                 "chrome, panels and viewport)", m_config.screenshotPath, w, h);
                    }
                    else
                    {
                        ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
                    }
                }

                m_captureWidth  = w;
                m_captureHeight = h;
                m_captureRgba   = std::move(actual);
                m_captureRead   = true;
                return true;
            }

            if (byteEqual && idle && !matches)
            {
                // Stable, quiescent, and WRONG -- keep spinning rather than
                // fail fast, the same reasoning RuntimeFrame::CaptureTail
                // gives (a non-_WIN32 IsIdle() stub can be vacuously true,
                // making the reference goal the only honest signal left).
                ARC_WARN("--compare attempt {}/{}: converged pixels do not match reference "
                         "'{}' -- {}", m_settleAttemptsUsed, m_config.settleAttempts,
                         m_config.compareReference, m_compareResult.errorMessage);
            }

            if (byteEqual && !idle)
            {
                // The pixels agree but the compiler is not done -- exactly
                // the case Task 10's runtime fix closes: NOT counted as
                // converged.
                ARC_WARN("--settle attempt {}/{}: frame {} matches frame {} byte-for-byte, but "
                         "the shader compiler is not idle yet (still pending/in-flight/"
                         "undrained) -- not counted as converged",
                         m_settleAttemptsUsed, m_config.settleAttempts, m_frameCount + 1,
                         m_frameCount);
            }

            // Not settled yet -- either the bytes genuinely differed, or they
            // matched but the compiler still has outstanding work. Either
            // way THIS frame becomes the baseline the NEXT attempt compares
            // against.
            m_previousCaptureWidth  = w;
            m_previousCaptureHeight = h;
            m_previousCaptureRgba   = std::move(actual);
            m_previousCaptureValid  = true;
        }

        // THE BAIL IS A CONJUNCTION (see Arcane/Host/SettleBound.hpp): both the
        // attempt budget AND the timeout must be spent. Line-for-line the same
        // decision RuntimeFrame::CaptureTail makes -- the two loops stay
        // equivalent, which is why the predicate lives in a shared header
        // rather than being spelled out twice.
        //
        // UNLIKE the runtime, this function ALWAYS returns true; the loop's
        // actual stop decision lives in EndFrame, which reads m_settleBail
        // (and m_settleAborted) rather than re-deriving a bound of its own.
        m_settleBail = Arcane::SettleBailDecision(m_settleAttemptsUsed, m_config.settleAttempts,
                                                  m_settleElapsedMs, m_config.settleTimeoutMs,
                                                  Arcane::kSettleIntervalMs);
        if (m_settleBail != Arcane::SettleBail::Keep)
        {
            // NON-CONVERGENCE. FAIL EXPLICITLY: m_settleConverged stays
            // false (EndFrame reads it to stop the loop; ShutdownGraphPath
            // reads it to fold exitReason "settle-not-converged"/
            // "compare-failed" into m_graphExit) and m_captureRead is
            // deliberately left false -- no --screenshot is written, and the
            // report's SetCapture is never called, so an agent reading
            // either artifact gets an honest ABSENCE rather than a frame
            // this run never actually agreed on.
            //
            // The GOVERNING bound is named, not merely the one that tripped, so
            // the caller knows which knob would actually change the outcome.
            ARC_ERROR("--settle: NOT CONVERGED after {} attempt(s) / {} ms -- two consecutive "
                      "captures never compared byte-equal with the compiler idle ({})",
                      m_settleAttemptsUsed, m_settleElapsedMs,
                      m_settleBail == Arcane::SettleBail::AttemptsBound
                          ? "attempt budget governed -- raise --settle"
                          : "timeout governed -- raise --settle-timeout");
        }
        else
        {
            // PACING. Without this the loop spins as fast as the chrome can
            // present, so the time bound could never be reached by anything but
            // a slow frame -- the interval is what makes the timeout a real
            // budget rather than a formality. 50ms is OURS, not inherited; see
            // kSettleIntervalMs.
            std::this_thread::sleep_for(std::chrono::milliseconds(Arcane::kSettleIntervalMs));
        }

        return true;
    }

    // Phase 19: present. One forward, to PresentChromeFrame -- the editor has
    // one presentation surface and one way to reach it.
    bool EditorApp::PresentFrame()
    {
        return PresentChromeFrame();
    }

    // Phase 20: end of frame -- plugin hot-reload poll + the --frames N budget.
    //
    // ===== THE DLL SWAP'S POSITION IS THE SAFETY ==========================
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
    // So at this statement nothing is recorded-but-unsubmitted.
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
    // handle at all), not of this ordering -- but the ordering is what
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
    //
    // THE VIEWPORT CAPTURE HAS NO FUNCTION OF ITS OWN EITHER. It is armed as a
    // NODE inside RenderSceneToViewport's own FrameDesc (vp.capture) and read
    // back synchronously right there -- there is no separate "read this
    // texture" entry point on the graph recorder for a later phase to use,
    // which is exactly the reasoning CaptureGraphViewportPng's own comment
    // gives for why an auto-screenshot re-renders rather than reading the last
    // frame.

    void EditorApp::EndFrame(LoopState& ls)
    {
        if (m_plugin) m_plugin->Poll();

        ++m_frameCount;

        // --settle N (Task 9). Once a settle loop is running, m_frameCount
        // reaching maxFrames no longer means "stop" -- it means "the base
        // budget is spent and PresentChromeFrame is now taking settle
        // attempts", which keeps happening every further iteration
        // (m_frameCount keeps growing past maxFrames the same way
        // RuntimeFrame::CaptureTail's io.frameCount does; nothing else in
        // this host reads "past maxFrames" as anything but a stop signal,
        // so that growth is harmless). The loop's ACTUAL end is decided by
        // PresentChromeFrame's settle branch alone, through
        // m_settleConverged / m_settleAttemptsUsed -- both set beside the
        // READBACK there, never off this counter (see PresentChromeFrame's
        // own comment on why: it returns false on a Skipped chrome present,
        // which never reaches EndFrame at all, so tying attempts to frame
        // advancement could spin forever on exactly that path).
        if (m_config.settleAttempts != 0)
        {
            // The bail is a CONJUNCTION now, and PresentChromeFrame has already
            // evaluated it into m_settleBail -- re-deriving an attempts-only
            // bound HERE would silently reinstate the very defect the shared
            // predicate exists to close, because this is where the editor's
            // loop actually stops (PresentChromeFrame always returns true).
            // m_settleAborted is the defensive fail-closed path's own stop, and
            // must be honoured unconditionally: it fires before any attempt is
            // ever taken, so neither bound of the conjunction can be spent.
            if (m_settleConverged || m_settleAborted || m_settleBail != Arcane::SettleBail::Keep)
                ls.running = false;
        }
        else if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames)
        {
            ls.running = false;
        }
    }
}
