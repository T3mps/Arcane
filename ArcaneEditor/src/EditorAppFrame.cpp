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

#include "EditorApp.hpp"
#include "EditorPanels.hpp"
#include "ViewportImGuiInput.hpp"

#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/PickBuffer.hpp>   // Arcane::PickBuffer (GPU hit-proxy viewport pick)
#include <Arcane/Render/SelectionOutline.hpp>   // Arcane::SelectionOutline (Edit-mode viewport outline)
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (gizmo drag target)
#include <Arcane/Scene/TransformSystems.hpp>   // Edit-mode derived-transform refresh
#include <Arcane/Serialization/SceneAsset.hpp>   // Arcane::Scene::kSceneExt (Save-dialog suffix)

#include <Astra/Registry/Registry.hpp>   // Registry::InspectEntity/GetComponent (gizmo descriptor resolve)

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
                    return ci.descriptor;
            }
            return nullptr;
        }
    }

    // The frame. Every line here is a phase call, and the ORDER IS THE
    // BEHAVIOUR -- see this file's header comment and each phase's own note.
    void EditorApp::MainLoop()
    {
        LoopState ls;
        ls.simPrev       = std::chrono::steady_clock::now();
        ls.lastFrameTime = ls.simPrev;

        while (ls.running)
        {
            const FramePump pump = PumpFrameEvents();
            if (pump == FramePump::Exit)      break;
            if (pump == FramePump::SkipFrame) continue;

            ConsumeDeferredSceneAction(ls);
            ConsumeSceneDialogResults(ls);
            ConsumeProjectDialogResult();
            ConsumeMaterialDialogResults();

            FrameState fs;
            FrameInput(ls, fs);
            AdvanceSim(ls);
            ApplyPendingViewportResize();
            UpdateScenePostChain();
            RenderSceneToViewport();
            CompositeGameUi();
            RenderSelectionOutline();
            PumpShaderEditor();
            DrawEditorUi(ls, fs);
            DrawModals(ls);
            DrawViewportPanelPhase(fs);
            SyncCenterTabFocus(fs);
            HandleViewportPick(fs);
            DrawSelectionPanels();
            if (!PresentFrame()) continue;
            EndFrame(ls);
        }
    }

    // Phase 1: pump the window/OS event queue. Must stay first -- everything
    // below reads the window size and the quit/minimize state it settles.
    EditorApp::FramePump EditorApp::PumpFrameEvents()
    {
        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested)
        {
            // Unsaved scene: park the quit behind the confirm modal rather than
            // dropping the user's work on a window close. Request returns false
            // when it parks, and this frame then runs normally so the modal draws.
            if (m_scene.Request(Arcane::Editor::SceneIntent::Exit, {}, *m_undo))
                return FramePump::Exit;
        }
        if (events.resized) m_gpu->OnResize(events.width, events.height);
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
    void EditorApp::RunSceneAction(const Arcane::Editor::SceneSession::PendingRequest& req,
                                   LoopState& ls)
    {
        switch (req.intent)
        {
            case Arcane::Editor::SceneIntent::NewScene:    DoNewScene(); break;
            case Arcane::Editor::SceneIntent::OpenScene:   DoOpenScene(req.path); break;
            case Arcane::Editor::SceneIntent::OpenProject: SwitchProject(req.path); break;
            case Arcane::Editor::SceneIntent::Exit:        ls.running = false; break;
            case Arcane::Editor::SceneIntent::None:        break;
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

    // Phase 3: scene file-dialog results. The thunks stash on an SDL BACKGROUND
    // thread; this is the main-thread consumption half and must stay at the top
    // of the frame, under the same mutex, never mid-render.
    void EditorApp::ConsumeSceneDialogResults(LoopState& ls)
    {
        // Scene file-dialog results (same background-thread stash pattern as the
        // project/material dialogs below).
        std::string sceneOpen, sceneSave;
        {
            std::lock_guard<std::mutex> lk(m_pendingSceneMutex);
            sceneOpen.swap(m_pendingSceneOpenPath);
            sceneSave.swap(m_pendingSceneSavePath);
        }
        if (!sceneOpen.empty())
        {
            // Guarded: opening over unsaved work parks the intent for the confirm
            // modal (drawn later this frame) instead of discarding it.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene, sceneOpen, *m_undo))
                DoOpenScene(sceneOpen);
        }
        if (!sceneSave.empty())
        {
            std::filesystem::path p = sceneSave;
            // The save dialog does not force the extension (Window.hpp), and a
            // scene without it does not scan as an asset. Case-insensitive:
            // a hand-typed "MyScene.ARCSCENE" must not become
            // "MyScene.ARCSCENE.arcscene".
            if (LowerExtension(p) != Arcane::Scene::kSceneExt)
                p += Arcane::Scene::kSceneExt;
            const bool saved = DoSaveScene(p);

            // This save may be the confirm modal's "Save" answer on a never-saved
            // scene: that branch launches this dialog and leaves the intent parked
            // until the path lands, which is now. Proceed only if the bytes
            // actually went to disk -- a failed save must not go on to discard the
            // work it was meant to preserve.
            if (m_scene.Pending() != Arcane::Editor::SceneIntent::None)
            {
                if (saved) RunSceneAction(m_scene.TakePending(), ls);
                else       m_scene.ClearPending();
            }
        }
    }

    // Phase 4: File->Open Project result. Same background-thread stash pattern;
    // the soft restart must land at the top of the frame, never mid-render.
    void EditorApp::ConsumeProjectDialogResult()
    {
        // File->Open Project: run a pending soft-restart at a safe point (top of
        // frame, never mid-render). Set by ProjectPickedThunk, which runs on an
        // SDL-owned background thread -- take the path out under the lock, then
        // switch on the local copy outside the lock (SwitchProject itself never
        // touches m_pendingProjectPath/m_pendingProjectMutex).
        std::string pending;
        {
            std::lock_guard<std::mutex> lk(m_pendingProjectMutex);
            pending.swap(m_pendingProjectPath);
        }
        if (!pending.empty())
        {
            // Same guard as Open Scene: the outgoing project's scene may have
            // unsaved changes, and switching would drop them.
            if (m_scene.Request(Arcane::Editor::SceneIntent::OpenProject, pending, *m_undo))
                SwitchProject(pending);
        }
    }

    // Phase 5: material/instance file-dialog results. Same background-thread
    // stash pattern; consumed here so document creation never lands mid-render.
    void EditorApp::ConsumeMaterialDialogResults()
    {
        // Material file-dialog results (same background-thread stash
        // pattern): create/open at the top of the frame, never mid-render.
        std::string materialNew, materialOpen, instanceNew;
        {
            std::lock_guard<std::mutex> lk(m_pendingMaterialMutex);
            materialNew.swap(m_pendingMaterialNewPath);
            materialOpen.swap(m_pendingMaterialOpenPath);
            instanceNew.swap(m_pendingInstanceNewPath);
        }
        if (!materialNew.empty())
            CreateMaterialAt(materialNew);
        if (!materialOpen.empty())
            m_documents.OpenPath(materialOpen);
        if (!instanceNew.empty())
            CreateInstanceAt(instanceNew, m_pendingInstanceParent);
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

        const auto now = std::chrono::steady_clock::now();
        const double frameDt = std::chrono::duration<double>(now - ls.lastFrameTime).count();
        ls.lastFrameTime = now;
        const Arcane::InputSnapshot snap =
            m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                      m_gpu->Imgui().WantCaptureMouse());

        // The plugin only sees scene-relevant input when the Viewport panel
        // is active (hovered/focused), with the cursor remapped into
        // viewport-local pixels (m_viewportRect/m_viewportActive are set at
        // the end of the PREVIOUS frame's ImGui pass -- see below). Otherwise
        // zero the mouse buttons/scroll so the plugin's camera does not pan
        // and spawn/drag does not fire while editing panels.
        Arcane::InputSnapshot pluginSnap = snap;
        float lx = 0, ly = 0;
        const bool inViewport =
            m_viewportActive &&
            Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, lx, ly);
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
        fs.gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
            m_play.IsPlaying(), inViewport, m_gameImgui->WantCaptureMouse());

        // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
        // game ImGui pass, which composites into the viewport AFTER this input
        // phase returns (see CompositeGameUi). Only the few values the game
        // context needs are hoisted -- the input phase stays narrow. Off the
        // viewport, hasInput is false so the game context reads the cursor as
        // off-target (BeginFrame injects -FLT_MAX).
        m_lastViewportMouse = glm::vec2(lx, ly);
        m_lastInViewport    = inViewport;
        m_lastMouseButtons  = snap.mouseButtons;
        m_lastWheel         = snap.wheelY;
        m_lastFrameDt       = frameDt;

        // Edit mode: the editor owns the left mouse button in the viewport
        // (click-pick + gizmo), so the hosted plugin must not also see it --
        // otherwise its LMB interactions (e.g. Sandbox spawn/drag/throw) fire
        // while editing. RMB + wheel stay live so plugin camera pan/zoom still
        // navigates the scene. (LMB=bit0; InputSnapshot.hpp.) ALSO clear it
        // when the game's viewport debug UI claims the pointer this frame
        // (Play + cursor over a game HUD widget) -- otherwise a HUD click
        // would fall through and spawn/drag gameplay underneath it.
        if (!m_play.IsPlaying() || fs.gameUiClaims)
            pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);
        // Edit mode: the EDITOR camera owns RMB-drag pan and wheel zoom
        // (see UpdateEditorCamera below), so the plugin must not see those
        // either. Both cameras write the SAME Runtime slot, and the
        // editor's push (before SubmitRender) wins in Edit mode -- so a
        // plugin still panning underneath would just be invisible work
        // whose result reappears the moment Play starts, from a viewpoint
        // the user never chose. RMB=bit1; InputSnapshot.hpp. Play is
        // untouched: the plugin keeps RMB + wheel and its camera wins.
        if (!m_play.IsPlaying())
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

    // Phase 6a: undo/redo + the Ctrl+N/O/S scene shortcuts. Runs EARLIER in the
    // frame than the gizmo phase below, which is why the open-transaction guard
    // is here rather than there.
    void EditorApp::HandleUndoRedoAndSceneShortcuts(const Arcane::InputSnapshot& snap,
                                                    FrameState& fs)
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
        const bool redoKeyDown = ctrl && ((shift && snap.ScancodeDown(kScZ)) ||
                                           (!shift && snap.ScancodeDown(kScY)));

        const bool active = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;
        // Also refuse while a transaction is open (e.g. a live gizmo drag):
        // CommandStack::Undo()/Redo() have no open-transaction guard, and this
        // keybind block runs earlier in the frame than the gizmo block below,
        // so an Undo here would apply the previous entry while the drag's own
        // "Gizmo" transaction is still holding pre-undo `before` bytes -- its
        // later Commit then pushes a transaction whose `before` predates the
        // undo and clobbers the redo entry. Ctrl is also the gizmo SNAP
        // modifier, so Ctrl-held drags are the normal case, not an edge case.
        const bool noOpenTxn = !m_undo->InTransaction();
        if (active && noOpenTxn && undoKeyDown && !m_prevUndoKeyDown) m_undo->Undo();
        if (active && noOpenTxn && redoKeyDown && !m_prevRedoKeyDown) m_undo->Redo();
        m_prevUndoKeyDown = undoKeyDown;
        m_prevRedoKeyDown = redoKeyDown;

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
        fs.scNewScene  = active && nDown && !m_prevKeyN;
        fs.scOpenScene = active && oDown && !m_prevKeyO;
        fs.scSaveScene = active && sDown && !m_prevKeyS;
        m_prevKeyN = nDown;
        m_prevKeyO = oDown;
        m_prevKeyS = sDown;
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
        const bool keysActive = !m_play.IsPlaying() && !snap.wantCaptureKeyboard && m_viewportActive;
        // Q = Select (no gizmo); W/E/R activate a transform gizmo (UE5 tools).
        if (keysActive && qDown && !m_prevKeyQ) m_gizmoEnabled = false;
        if (keysActive && wDown && !m_prevKeyW) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Translate; }
        if (keysActive && eDown && !m_prevKeyE) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Rotate; }
        if (keysActive && rDown && !m_prevKeyR) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Scale; }
        m_prevKeyW = wDown;
        m_prevKeyE = eDown;
        m_prevKeyR = rDown;
        m_prevKeyQ = qDown;
    }

    // Phase 6c: editor viewport camera. Its tail holds the FIRST of the frame's
    // two SetCamera pushes -- see the comment there; the gizmo phase below reads
    // what it writes.
    void EditorApp::UpdateEditorCamera(const Arcane::InputSnapshot& snap, bool inViewport,
                                       float lx, float ly)
    {
        // Editor viewport camera (Edit mode): RMB-drag pans, wheel zooms
        // at the cursor, F frames the selection (everything when nothing
        // is selected), Home frames everything. The plugin no longer sees
        // RMB/wheel in Edit mode (see the pluginSnap mask above), so the
        // two cameras cannot fight over the same gesture.
        const bool      rmbDown = (snap.mouseButtons & 0x2u) != 0;
        const glm::vec2 mouseWindow(snap.mouseX, snap.mouseY);
        if (!m_play.IsPlaying())
        {
            // A pan may only START over the viewport, but once
            // started it keeps tracking anywhere -- same rule as the
            // gizmo drag, so crossing the panel edge mid-drag does
            // not strand the view.
            if (rmbDown && !m_prevRmbDown && inViewport)
                m_camPanning = true;
            if (!rmbDown)
                m_camPanning = false;
            // RMB held across BOTH frames, so m_camPanLastMouse is a
            // real previous cursor and the press edge cannot jump the
            // view by the whole distance from wherever the cursor last
            // was (same guard as Sandbox's Interaction pan).
            if (m_camPanning && m_prevRmbDown)
                m_camera.Pan(mouseWindow - m_camPanLastMouse);

            // Zoom anchors on the viewport-local cursor, the space
            // the camera offset itself lives in. Deliberately NOT
            // gated on snap.wantCaptureMouse: it is true over the
            // viewport image by design (see the pluginSnap comment
            // above); inViewport already folds in m_viewportActive,
            // which is false whenever another panel owns the cursor.
            if (inViewport && snap.wheelY != 0.0f)
                m_camera.ZoomAt(glm::vec2(lx, ly), snap.wheelY);
        }
        else
        {
            m_camPanning = false;   // Play owns the pointer; drop any live pan
        }
        m_camPanLastMouse = mouseWindow;
        m_prevRmbDown     = rmbDown;

        // F / Home framing. Gated on wantCaptureKeyboard exactly like
        // the Ctrl+N/O/S shortcuts above, so F does not fire while a
        // text field or the Outliner rename box has focus. Unlike the
        // W/E/R gizmo keys this does NOT require viewport focus:
        // those switch a viewport TOOL, while framing acts on the
        // selection, and picking an entity in the Outliner and
        // pressing F is the point of the shortcut.
        const bool fDown    = snap.ScancodeDown(kScF);
        const bool homeDown = snap.ScancodeDown(kScHome);
        const bool framingActive = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;
        if (framingActive && fDown && !m_prevKeyF)
            FrameCamera(m_selection.HasSelection());
        if (framingActive && homeDown && !m_prevKeyHome)
            FrameCamera(false);
        m_prevKeyF    = fDown;
        m_prevKeyHome = homeDown;

        // Push the editor camera BEFORE the gizmo block below reads
        // Runtime::CameraOffset/CameraZoom. Those reads happen earlier
        // in the frame than the pre-SubmitRender push further down, so
        // without this the gizmo would hit-test against whatever camera
        // was stored LAST frame -- and on the very first frame against
        // the identity (zoom 1), placing the handles 100x off the
        // sprite. The click-pick and gizmo DRAW sites run after the
        // later push and are already consistent with it.
        if (!m_play.IsPlaying())
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
        const bool lmbDown          = (snap.mouseButtons & 0x1u) != 0;
        const bool mousePressedLeft  = lmbDown && !m_prevLmbDown;
        const bool mouseReleasedLeft = !lmbDown && m_prevLmbDown;
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
        const bool gizmoActive = !m_play.IsPlaying() && !gameUiClaims &&
                                 m_gizmoEnabled && m_selection.HasSelection() &&
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

        m_prevLmbDown = lmbDown;
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
        // Apply the viewport panel's size from LAST frame BEFORE rendering the scene,
        // so this frame's ImGui::Image() captures a texture that is not destroyed later
        // in the same frame (OffscreenCanvas::Resize synchronously frees the old texture).
        if (m_pendingViewportW != 0 && m_pendingViewportH != 0 &&
            (m_pendingViewportW != m_viewport->Width() || m_pendingViewportH != m_viewport->Height()))
        {
            m_viewport->Resize(m_pendingViewportW, m_pendingViewportH);
            m_pick->Resize(m_pendingViewportW, m_pendingViewportH);
            m_outline->Resize(m_pendingViewportW, m_pendingViewportH);
        }
    }

    // Phase 9: post-chain sweep + the Edit-mode derived-transform refresh +
    // the SECOND camera push. All three sit immediately before the scene
    // submit in the next phase, and each says below why it has to.
    void EditorApp::UpdateScenePostChain()
    {
        // Scene post chain (post arc, slice 3): sweep for the assignment and
        // feed the viewport hook from the CURRENT cache state. The FIRST
        // entity with a valid PostProcess.material wins (>1 warns once);
        // none/unresolved leaves the hook null -- today's path. Chain/
        // Instance are re-fetched every frame BEFORE Draw: last frame's
        // drain may have swapped the bound instance under a re-save.
        Arcane::Guid postId{};
        int postCount = 0;
        m_runtime->Registry().CreateView<Arcane::PostProcess>().ForEach(
            [&](Astra::Entity, Arcane::PostProcess& pp)
        {
            if (!pp.material.IsValid())
                return;
            if (postCount++ == 0)
                postId = pp.material;
        });
        if (postCount > 1)
        {
            if (!m_warnedMultiPost)
                ARC_WARN("scene carries {} PostProcess assignments -- "
                         "the first found wins", postCount);
            m_warnedMultiPost = true;
        }
        else
        {
            m_warnedMultiPost = false;
        }

        Arcane::FullscreenMaterialChain* postChain = nullptr;
        const Arcane::MaterialInstance* postInst = nullptr;
        if (postId.IsValid() && m_postChains)
        {
            m_postChains->Request(postId, m_editorClock);
            postChain = m_postChains->Chain(postId);
            postInst = m_postChains->Instance(postId);
        }
        Arcane::GlobalParams postGlobals;
        postGlobals.time = (float)m_editorClock;
        postGlobals.deltaTime = (float)m_lastFrameDt;
        postGlobals.viewportWidth = (float)m_viewport->Width();
        postGlobals.viewportHeight = (float)m_viewport->Height();
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
        if (!m_play.IsPlaying())
        {
            Arcane::TransformPropagationSystem{}(m_runtime->Registry());
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
        // Play mode deliberately does NOT push: the plugin's camera wins
        // so the game looks like the game.
        if (!m_play.IsPlaying())
            m_runtime->SetCamera(m_camera.offset, m_camera.zoom);

        m_viewport->SetPostGlobals(postGlobals);
        m_viewport->SetPostChain(postChain, postInst,
                                 &m_runtime->AssetsFacade());
    }

    // Phase 10: the scene render. Must run BEFORE the ImGui pass builds the
    // Viewport panel's Image, and the gizmo inside it is drawn AFTER the scene
    // submit so it renders on top.
    void EditorApp::RenderSceneToViewport()
    {
        // Scene -> offscreen canvas (the SAME canvas->batcher->tonemap path ArcaneRuntime
        // drives, but into a panel texture). SetRenderContext writes RenderContext2D
        // in Arcane.dll and applies the plugin's stored camera; SubmitRender runs the
        // render scheduler (sprite submission + physics debug overlay) into this
        // batcher. Runs BEFORE ImGui builds the Viewport panel's Image so the texture
        // is ready when ImGui samples it at backbuffer-render time.
        m_viewport->Draw(
            [&](Arcane::Batcher2D& b)
            {
                // Globals for registered sprite materials (Time/Delta/
                // Viewport); built-in pipelines ignore them.
                Arcane::GlobalParams sceneGlobals;
                sceneGlobals.time = (float)m_editorClock;
                sceneGlobals.deltaTime = (float)m_lastFrameDt;
                sceneGlobals.viewportWidth = (float)m_viewport->Width();
                sceneGlobals.viewportHeight = (float)m_viewport->Height();
                b.SetGlobals(sceneGlobals);

                m_runtime->SetRenderContext(&b);
                m_runtime->Loop().SubmitRender();

                // Transform gizmo, drawn AFTER the scene submit so it renders on
                // top. Frame ordering guarantees the input block above already ran
                // this frame, so m_gizmoHovered/m_gizmoDrag and the live
                // Transform read here are current. Edit-mode + has-selection
                // only (mirrors the interaction gate; no viewport-hover requirement
                // here -- the gizmo should stay visible while e.g. the mouse is over
                // the Inspector, just not be interactable there).
                if (!m_play.IsPlaying() && m_gizmoEnabled && m_selection.HasSelection())
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
            },
            glm::vec4(0.02f, 0.02f, 0.04f, 1.0f));
    }

    // Phase 11: the game's own ImGui HUD, composited into the viewport texture.
    // Runs AFTER the scene render above and BEFORE the editor's BeginFrame below.
    void EditorApp::CompositeGameUi()
    {
        // Game debug UI -> the plugin's OWN ImGui context, composited into the
        // viewport's output texture (Play only). Runs the plugin's DrawUI in the
        // GAME context and blends it OVER the tonemapped scene -- so the HUD lives
        // inside the Viewport panel, never over the editor chrome. Runs AFTER
        // m_viewport->Draw (scene -> tonemap -> output texture) and BEFORE the
        // editor's BeginFrame below. The game context is fully bracketed by the
        // OffscreenImGuiLayer calls (each SetCurrentContext internally), so the
        // editor context is untouched. Edit mode: not run (clean viewport); the
        // input is reset so a stray draw sees no cursor/buttons.
        if (!m_play.IsPlaying())
            m_gameImgui->SetInput({});
        const Arcane::PluginVTable* vtGame = m_plugin ? m_plugin->Vtable() : nullptr;
        if (m_play.IsPlaying() && vtGame && vtGame->DrawUI)
        {
            Arcane::OffscreenImGuiLayer::Input gi;
            gi.displaySize  = glm::vec2((float)m_viewport->Width(), (float)m_viewport->Height());
            gi.deltaTime    = (float)m_lastFrameDt;
            gi.hasInput     = m_lastInViewport;
            gi.mousePos     = m_lastViewportMouse;
            gi.mouseDown[0] = (m_lastMouseButtons & 0x1u) != 0;   // LMB
            gi.mouseDown[1] = (m_lastMouseButtons & 0x2u) != 0;   // RMB
            gi.mouseDown[2] = (m_lastMouseButtons & 0x4u) != 0;   // MMB
            gi.wheel        = m_lastWheel;
            m_gameImgui->SetInput(gi);
            m_gameImgui->BeginFrame();
            m_plugin->DrawUIAll();   // game module + any secondary plugins, into the game context
            // Composite the game HUD into the viewport output texture on a one-off
            // command list (mirrors the backbuffer pass below). The OffscreenCanvas
            // owns a SEPARATE command list for its scene pass, so m_gpu->Cmd() is
            // idle here; NVRHI auto-transitions the output texture (RenderTarget for
            // this pass, back to ShaderResource for the editor's ImGui::Image).
            m_gpu->Cmd()->open();
            m_gameImgui->Render(m_gpu->Cmd(), m_viewport->OutputFramebuffer());
            m_gpu->Cmd()->close();
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
        }
    }

    // Phase 12: Edit-mode selection/hover outline. Occupies the same slot as the
    // Play-only game-imgui composite above -- the two are mutually exclusive by
    // mode -- and must stay after the scene render, before the editor ImGui pass.
    void EditorApp::RenderSelectionOutline()
    {
        // Selection + hover outline -> the viewport's own layer (Edit only). Refreshes
        // the hit-proxy id buffer, then edge-detects it into the post-tonemap output
        // texture (amber selected, cyan hovered). Skipped entirely when there is nothing
        // to outline. Play mode: not run (the game-imgui overlay owns this slot instead,
        // see above -- the two are mutually exclusive by mode).
        if (!m_play.IsPlaying() && (m_selection.HasSelection() || m_lastInViewport))
        {
            const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
            m_pick->RenderIdPass(m_runtime->Registry(), view);

            // Every selected entity that made it into this frame's id pass.
            // SelectionOutline caps and warns; no clamping needed here.
            std::vector<uint32_t> selectedIds;
            selectedIds.reserve(m_selection.Count());
            for (Astra::Entity e : m_selection.Entities())
            {
                const uint32_t id = m_pick->PassIdOf(e);
                if (id != 0u)
                    selectedIds.push_back(id);
            }

            Arcane::SelectionOutline::Params op;
            op.selectedIds = selectedIds;
            op.cursorPx   = m_lastInViewport
                          ? glm::ivec2((int)m_lastViewportMouse.x, (int)m_lastViewportMouse.y)
                          : glm::ivec2(-1, -1);

            m_gpu->Cmd()->open();
            m_outline->Render(m_gpu->Cmd(), m_pick->IdTarget(),
                              m_viewport->OutputFramebuffer(), op);
            m_gpu->Cmd()->close();
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
        }
    }

    // Phase 13: the compile-service pump. This is the ONE drain site where
    // compile results become NVRHI shaders; it also advances m_editorClock,
    // which every phase above reads as "this frame's time".
    void EditorApp::PumpShaderEditor()
    {
        // Shader-editor pump (Slice 5): advance the compile clock, dispatch
        // due jobs, and route drained results to their documents AND the
        // sprite-material cache -- the ONE drain site where compile results
        // become NVRHI shaders. Then tick every document (preview render on
        // its own OffscreenCanvas).
        m_editorClock += m_lastFrameDt;
        // Sprite-asset arc, Task 3 review fix (F1): compilerAvailable now
        // gates ONLY the material path (Request + the Poll/Drain/Consume
        // block below), not the sprite-asset sweep. It used to wrap the
        // whole sweep, but SpriteCache::Request has no compile-pipeline
        // dependency at all -- it is a synchronous JSON-load + Assets-facade
        // resolve -- and ShaderCompiler::IsAvailable() (ShaderCompiler.hpp:
        // 108-113) is literally "did dxcompiler.dll/dxil.dll load". Under
        // the old gate, a DXC-less machine would silently never resolve ANY
        // sprite (not even the negative-result default entry), so every
        // sprite rendered as an indistinguishable untextured 1x1 tint quad
        // with no diagnostic. The material path's own condition is
        // unchanged (still exactly `m_shaderCompiler &&
        // m_shaderCompiler->IsAvailable()`, just named instead of inlined),
        // so material behavior is byte-identical to before this fix.
        const bool compilerAvailable = m_shaderCompiler && m_shaderCompiler->IsAvailable();

        // Sweep the scene once per frame for BOTH per-SpriteRenderer Guids --
        // one CreateView<SpriteRenderer> walk covers s.material (Slice 8,
        // gated on compilerAvailable) and s.sprite (sprite-asset arc Task 3,
        // ungated) so this fix does not add a second full component-view
        // pass per frame. Request no-ops once a Guid is known either way, so
        // this is a cheap per-frame guarantee that whatever the scene
        // references is compiling/bound (materials) or resolved (sprites).
        if (m_spriteMaterials || m_sprites)
        {
            auto sprites = m_runtime->Registry().CreateView<Arcane::SpriteRenderer>();
            sprites.ForEach([&](Astra::Entity, Arcane::SpriteRenderer& s)
            {
                if (compilerAvailable && m_spriteMaterials && s.material.IsValid())
                    m_spriteMaterials->Request(s.material, m_editorClock);
                if (m_sprites && s.sprite.IsValid())
                    m_sprites->Request(s.sprite);
            });
        }

        if (compilerAvailable)
        {
            m_shaderCompiler->Poll(m_editorClock);
            for (const Arcane::ShaderCompileResult& r : m_shaderCompiler->Drain())
            {
                bool consumed = false;
                m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
                {
                    if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d))
                        consumed = doc->ConsumeResult(r) || consumed;
                });
                if (!consumed && m_spriteMaterials)
                    consumed = m_spriteMaterials->ConsumeResult(r, m_viewport->Batch());
                if (!consumed && m_postChains)
                    m_postChains->ConsumeResult(r);
            }
        }
        // Publish the resolution table every frame (the map's address is
        // stable; re-setting keeps the resource honest across project
        // switches and registry swaps).
        if (m_spriteMaterials)
            m_runtime->SetSpriteMaterials(&m_spriteMaterials->Table());
        if (m_sprites)
            m_runtime->SetSpriteTable(&m_sprites->Table());
        PollMaterialWatch();   // external .arcmat edits (~1 Hz mtime sweep)
        m_documents.TickAll(m_lastFrameDt);
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
        Arcane::Editor::BeginDockSpace(*m_undo, menuReq, m_scene.IsDirty(*m_undo),
                                       m_play.IsPlaying());
        Arcane::Editor::DrawSimTimeToolbar(m_play, *m_runtime, m_plugin ? m_plugin->Vtable() : nullptr,
                                           (uint64_t)(intptr_t)m_toolbarLogo.Get());
        Arcane::Editor::EndDockSpace();
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
            m_gpu->Win().ShowOpenFileDialog(&EditorApp::ProjectPickedThunk, this,
                                            "Arcane Project", "arcproj");
        if (menuReq.newMaterial || menuReq.openMaterial)
        {
            // Material dialogs start in the project's Content/ (the only place
            // a saved asset can register + resolve by GUID); no project = OS default.
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const std::string contentDir =
                proj ? (proj->Root() / "Content").string() : std::string();
            const char* defaultPath = contentDir.empty() ? nullptr : contentDir.c_str();
            if (menuReq.newMaterial)
                m_gpu->Win().ShowSaveFileDialog(&EditorApp::MaterialNewPickedThunk, this,
                                                "Arcane Material", "arcmat", defaultPath);
            if (menuReq.openMaterial)
                m_gpu->Win().ShowOpenFileDialog(&EditorApp::MaterialOpenPickedThunk, this,
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
            m_gpu->Win().ShowOpenFileDialog(&EditorApp::SceneOpenPickedThunk, this,
                                            "Arcane Scene", "arcscene",
                                            dir.empty() ? nullptr : dir.c_str());
        }
        // Save on a never-saved scene IS Save As -- otherwise the item would look
        // enabled and do nothing.
        if (menuReq.saveScene && !m_scene.Path().empty())
            DoSaveScene(m_scene.Path());
        if (menuReq.saveSceneAs || (menuReq.saveScene && m_scene.Path().empty()))
            ShowSceneSaveDialog();

        const Arcane::Editor::AssetBrowserActions browserActions =
            Arcane::Editor::DrawAssetBrowserPanel(m_assetBrowser,
                                                  m_runtime->CurrentProject(), m_documents);
        if (browserActions.createInstanceOf.IsValid())
        {
            m_pendingInstanceParent = browserActions.createInstanceOf;
            const Arcane::Project* proj = m_runtime->CurrentProject();
            const std::string contentDir =
                proj ? (proj->Root() / "Content").string() : std::string();
            m_gpu->Win().ShowSaveFileDialog(&EditorApp::InstanceNewPickedThunk, this,
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
                m_sceneError = "Could not write the project's boot scene (see Console).";
        }
        Arcane::Editor::DrawConsolePanel(m_console);

        // The Material panel draws BEFORE the documents, and that order is
        // load-bearing rather than incidental: the panel's param rows and the
        // document's graph widgets open gestures against the SAME
        // EditGesture::GestureState, and each scope's ScopeGuard closes an
        // abandoned one when it destructs. Drawing the panel first leaves
        // ShaderEditorDocument::Draw's guard as the LAST one to run each frame,
        // so a gesture the panel opened is never force-closed by the document
        // before the panel's own EndOnDeactivate has had its say.
        //
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
        Arcane::Editor::DrawMaterialPanel(activeMat);

        // New documents tab into the Viewport's node (captured last frame).
        m_documents.DrawAll(m_viewportDockId);
    }

    // Phase 15: the modals. Opened and drawn at DOCKSPACE level, outside any
    // panel window -- that is why they live here and not inside a panel draw.
    void EditorApp::DrawModals(LoopState& ls)
    {
        // Project-open failure modal: any refusal in SwitchProject/Init lands
        // here (drawn at the dockspace level, outside any panel window). The
        // string stays set until the user dismisses it, so OpenPopup re-arms
        // across frames even if another popup momentarily owned the stack.
        if (!m_projectOpenError.empty() && !ImGui::IsPopupOpen("Open Project Failed"))
            ImGui::OpenPopup("Open Project Failed");
        if (ImGui::BeginPopupModal("Open Project Failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(m_projectOpenError.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                m_projectOpenError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Unsaved-scene confirm. SceneSession parked the intent (New Scene, Open
        // Scene, Open Project, Exit) because the scene is dirty; this popup is
        // where it is answered. Same re-arm shape as the modal above.
        if (m_scene.Pending() != Arcane::Editor::SceneIntent::None &&
            !ImGui::IsPopupOpen("Unsaved Scene"))
            ImGui::OpenPopup("Unsaved Scene");
        if (ImGui::BeginPopupModal("Unsaved Scene", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (m_scene.Pending() == Arcane::Editor::SceneIntent::None)
            {
                // The intent was taken OUTSIDE this popup: the Save button's
                // never-saved branch launches a file dialog and leaves the popup
                // up, and the save (and with it the action) lands at the top of a
                // later frame. Nothing left to ask about.
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ImGui::TextUnformatted(("'" + m_scene.DisplayName() +
                                        "' has unsaved changes.").c_str());
                ImGui::Separator();
                if (ImGui::Button("Save", ImVec2(90, 0)))
                {
                    // Stop Play BEFORE saving, in both branches below. Stop
                    // restores the pre-Play snapshot, which IS the authored
                    // state -- so what reaches disk is exactly what the user
                    // believes they are saving, and DoSaveScene's Play refusal
                    // (which would otherwise turn this button into an error
                    // popup) is satisfied. Not a surprising side effect: no
                    // intent parkable here leaves Play running anyway -- New
                    // Scene, Open Scene and Open Project all stop it through
                    // ClearSceneReferences, and Exit ends the process.
                    if (m_play.IsPlaying())
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
                        // The save failed (m_sceneError says why). Drop the action
                        // rather than proceed -- proceeding would discard exactly
                        // the work the save was meant to preserve.
                        m_scene.ClearPending();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard", ImVec2(90, 0)))
                {
                    ls.sceneAction = m_scene.TakePending();
                    ImGui::CloseCurrentPopup();
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

        // Scene failure modal (bad file, failed write). Same re-arm shape.
        if (!m_sceneError.empty() && !ImGui::IsPopupOpen("Scene Error"))
            ImGui::OpenPopup("Scene Error");
        if (ImGui::BeginPopupModal("Scene Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(m_sceneError.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                m_sceneError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Phase 16: the Viewport panel itself. It samples the texture the render
    // phases produced, and publishes the rect/active/size the NEXT frame's
    // input phase and resize phase read (the deliberate one-frame lag).
    void EditorApp::DrawViewportPanelPhase(FrameState& fs)
    {
        fs.vp = Arcane::Editor::DrawViewportPanel(m_viewport->TextureId(),
                                            m_viewport->Width(), m_viewport->Height(),
                                            m_gizmoEnabled, m_gizmoMode, m_gizmoSpace);
        m_viewportDockId = fs.vp.dockId;
        m_pendingViewportW = fs.vp.desiredW;
        m_pendingViewportH = fs.vp.desiredH;
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
        // Viewport click-pick: GPU hit-proxy. Render every pickable entity's
        // silhouette into the id buffer and read back the pixel under the
        // viewport-local click. `view` is the SAME world->canvas transform the
        // scene render uses (m_runtime's camera == RenderContext2D), so the id
        // silhouettes register pixel-for-pixel with what is drawn. An invalid
        // result (background / outside the viewport) clears the selection.
        // Suppressed when this frame's click was already consumed by the gizmo
        // (pressed/released a handle) or a drag is still in progress -- otherwise
        // clicking/using a handle would also re-pick and disturb the selection.
        // Also suppressed when the game's viewport debug UI claimed the pointer
        // this frame (gameUiClaims, computed in the input phase above) so a click
        // on a game HUD widget does not also re-pick the editor selection.
        if (fs.vp.clicked && !m_gizmoCapturedClick && !m_gizmoDrag.active && !fs.gameUiClaims)
        {
            const Arcane::PickView view{ m_runtime->CameraOffset(),
                                         m_runtime->CameraZoom() };
            const Astra::Entity picked = m_pick->Pick(
                m_runtime->Registry(), view,
                glm::vec2(fs.vp.clickLocalX, fs.vp.clickLocalY));
            if (picked.IsValid())
            {
                if (fs.vp.ctrlHeld)
                    m_selection.Toggle(picked);
                else
                    m_selection.Select(picked);
            }
            else if (!fs.vp.ctrlHeld)
            {
                // Ctrl+click on empty space is a miss, not a deselect-all --
                // otherwise one stray click discards a built-up selection.
                m_selection.Clear();
            }
        }
    }

    // Phase 18: the selection-driven panels. Runs after the click-pick above so
    // they draw THIS frame's selection.
    void EditorApp::DrawSelectionPanels()
    {
        m_selection.Prune([reg = &m_runtime->Registry()](Astra::Entity e)
                          { return reg->IsValid(e); });
        m_editBinding.editMode = !m_play.IsPlaying();
        Arcane::Editor::DrawOutlinerPanel(m_runtime->Registry(), m_selection,
                                          *m_undo, m_editBinding, m_outliner);
        Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo,
                                           m_editBinding, m_runtime->CurrentProject(),
                                           m_inspector, &m_inspectorServices);

        // (The hosted plugin's DrawUI now renders into its OWN ImGui context,
        // composited into the viewport texture above -- not the editor context.)
    }

    // Phase 19: present. Returns false when the swapchain had no backbuffer, in
    // which case MainLoop skips the rest of the frame -- exactly as the original
    // `continue` did, so the plugin Poll + frame count below are skipped too.
    bool EditorApp::PresentFrame()
    {
        nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
        if (!backbuffer) { ImGui::EndFrame(); return false; }

        m_gpu->Cmd()->open();
        // Clear the backbuffer directly (Arcane Editor's scene will live in a panel,
        // so there is no scene->tonemap->backbuffer pass as in ArcaneRuntime).
        m_gpu->Cmd()->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                        nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
        nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
        m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
        m_gpu->Cmd()->close();
        m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
        m_gpu->Swap().Present();
        return true;
    }

    // Phase 20: end of frame -- plugin hot-reload poll + the --frames N budget.
    void EditorApp::EndFrame(LoopState& ls)
    {
        if (m_plugin) m_plugin->Poll();

        ++m_frameCount;
        if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) ls.running = false;
    }
}
