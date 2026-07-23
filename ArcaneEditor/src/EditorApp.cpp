// EditorApp: Init -> MainLoop -> Shutdown. Reuses Loom's host-boot helpers
// (GpuContext/FramePerf/LoomConfig) by source-compile and hosts Sandbox.dll via
// the lifted Arcane::PluginHost. The frame loop advances the sim through the
// RunLoop, renders the scene into an OffscreenCanvas (the same canvas->batcher->
// tonemap path Loom drives, into a panel texture instead of the backbuffer), and
// draws an editor shell -- a full-viewport dockspace (Arcane::Editor::BeginDockSpace)
// hosting a Sim toolbar (play/pause/step + time-scale), a Console panel fed by a
// callback sink on Arcane::Log::Engine(), and a dockable Viewport panel showing
// the scene texture. Scene input (camera pan/zoom, click-pick) is gated on the
// Viewport panel's hover/focus and the cursor is remapped into viewport-local
// pixels before the plugin sees it (see ViewportInput.hpp). The render plumbing
// + teardown order live in GpuContext (m_gpu). The teardown CONTRACT is encoded
// in the EditorApp member declaration order -- see EditorApp.hpp.

#include "EditorApp.hpp"
#include "EditorFonts.hpp"
#include "EditorPanels.hpp"
#include "ViewportImGuiInput.hpp"

#include <ProjectBoot.hpp>
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/PickBuffer.hpp>   // Arcane::PickBuffer (GPU hit-proxy viewport pick)
#include <Arcane/Render/SelectionOutline.hpp>   // Arcane::SelectionOutline (Edit-mode viewport outline)
#include <Arcane/Scene/Components.hpp>   // Arcane::LocalTransform (gizmo drag target)

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>   // Registry::InspectEntity/GetComponent (gizmo descriptor resolve)

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

namespace Arcane::Editor
{
    namespace
    {
        // Undo/redo keybind scancodes (see MainLoop's input block). Hoisted to file
        // scope -- pure cleanup, values unchanged (verified against SDL_scancode.h).
        constexpr uint32_t kScLCtrl  = 224;  // SDL_SCANCODE_LCTRL
        constexpr uint32_t kScRCtrl  = 228;  // SDL_SCANCODE_RCTRL
        constexpr uint32_t kScLShift = 225;  // SDL_SCANCODE_LSHIFT
        constexpr uint32_t kScRShift = 229;  // SDL_SCANCODE_RSHIFT
        constexpr uint32_t kScY      = 28;   // SDL_SCANCODE_Y
        constexpr uint32_t kScZ      = 29;   // SDL_SCANCODE_Z

        // Gizmo mode-switch keybind scancodes (see MainLoop's input block). No
        // conflict with the Sandbox plugin's camera (RMB-pan + wheel-zoom only).
        constexpr uint32_t kScW = 26;   // SDL_SCANCODE_W
        constexpr uint32_t kScE = 8;    // SDL_SCANCODE_E
        constexpr uint32_t kScR = 21;   // SDL_SCANCODE_R
        constexpr uint32_t kScQ = 20;   // SDL_SCANCODE_Q

        // Resolve the ComponentDescriptor for LocalTransform on `e`, for bracketing
        // a gizmo drag into the undo stack via CommandStack::SnapshotComponent.
        // Mirrors EditorPanels.cpp's Inspector loop (InspectEntity + meta->typeName
        // match) -- namespace-qualified, matching how ASTRA_REFLECT_TYPE registers it.
        const Astra::ComponentDescriptor* FindLocalTransformDescriptor(Astra::Registry& reg, Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            {
                if (ci.meta && ci.meta->typeName == "Arcane::LocalTransform")
                    return ci.descriptor;
            }
            return nullptr;
        }

        // Window title: the project name when a project is open, else the bare
        // editor name (no --project => legacy data/-next-to-exe boot, unchanged).
        std::string EditorTitle(const Arcane::Project* project)
        {
            if (project)
                return "Arcane Editor -- " + project->Manifest().name;
            return "Arcane Editor";
        }
    }

    EditorApp::EditorApp(LoomConfig cfg)
        : m_config(std::move(cfg)), m_perf(m_config.perf) {}

    bool EditorApp::Init()
    {
        // The whole platform/render/input stack, booted in order. Owned by m_gpu and
        // declared BEFORE m_runtime/m_plugin in EditorApp -- so it destructs AFTER
        // them: the render resources it owns (window/device/swapchain/shaders/canvas/
        // batcher/tonemap/imgui/input + commandList/framebuffers) must outlive runtime
        // + plugin.
        m_gpu = GpuContext::Create(m_config);
        if (!m_gpu)
        {
            ARC_ERROR("Arcane Editor: GPU context create failed");
            return false;
        }

        ARC_INFO("{} -- Arcane Editor host, backend {}", Arcane::BuildInfo(), Arcane::ToString(m_config.backend));

        // Title the window as the editor. GpuContext defaults to "Arcane Loom" (the
        // shared host helper Loom also uses); override it here so only this host reads
        // "Arcane Editor" -- Loom keeps its own title.
        m_gpu->Win().SetTitle("Arcane Editor");

        // Editor shell: enable ImGui docking (the placeholder single window becomes
        // a dockspace + panels in MainLoop) and route the engine logger into the
        // Console panel's ring buffer.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        InstallConsoleSink();

        // Editor fonts: Roboto base + merged lucide icons, on the editor context
        // (current here -- the only ImGui context created so far, see GpuContext::
        // Create's ImGuiLayer::Create above), before the first frame and before the
        // game ImGui context is created below. Zero engine change.
        Arcane::Editor::InstallEditorFonts();

        // The TypeContext is the process-wide type-identity singleton shared across
        // ArcaneEditor.exe, Arcane.dll, and every loaded plugin. It is intentionally
        // heap-allocated and never freed: TypeMeta entries registered by the plugin
        // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
        // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
        // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
        // ever destructed, ~std::function() would invoke those thunks -> crash.
        // Heap-leaking is the correct production pattern for a long-running host;
        // the OS reclaims all process memory on exit anyway.
        m_typeContext = new Astra::TypeContext();
        // Install the shared context in THIS module too (ArcaneEditor.exe is a separate
        // binary from Arcane.dll -- Astra::GetTypeContext()/SetTypeContext() resolve
        // through a PER-MODULE static slot, by design; Runtime::Impl's ctor installs
        // the same m_typeContext for Arcane.dll's own slot, see Runtime.cpp). Required
        // BEFORE the gizmo interaction code's TypeID<Arcane::LocalTransform>::Value()
        // lookups (Registry::GetComponent<LocalTransform> in MainLoop) -- without this,
        // ArcaneEditor.exe's first TypeID<T>::Value() call would silently fall back to its
        // own empty module-local DefaultTypeContext() instead of the shared one, so
        // GetComponent<LocalTransform> would resolve against the WRONG ComponentID
        // (always-miss at best, aliasing a different component's bytes at worst).
        Astra::SetTypeContext(m_typeContext);
        // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
        // until quit). The scripted "ArcaneEditor --frames N" GPU-verify is headless -> false
        // -> miniaudio's null backend (no real device grabbed on a CI box).
        m_runtime.emplace(m_typeContext, m_config.maxFrames == 0);

        // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
        // Runtime so a plugin can build its own engine render objects (e.g. the
        // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
        // plugin (m_gpu is declared before the runtime/plugin in EditorApp). Null in
        // a headless host -> the plugin skips its GPU-resource creation.
        m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

        // Open the project (if any) BEFORE loading input + the game module (mirrors Loom).
        if (!m_config.projectPath.empty())
        {
            if (!m_runtime->OpenProject(m_config.projectPath))
                ARC_WARN("Arcane Editor: --project '{}' failed to open; using data/ + --plugin fallback",
                         m_config.projectPath);
        }
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->CurrentProject()))
            ARC_WARN("Arcane Editor: input actions failed to load");
        m_gpu->Win().SetTitle(EditorTitle(m_runtime->CurrentProject()));

        // The hosted plugin draws its debug UI into its OWN "game" ImGui context,
        // composited INTO the viewport texture (see MainLoop), instead of the
        // editor context where a HUD would float over the editor chrome. Created
        // here, AFTER the editor ImGui layer is up (GpuContext::Create) and BEFORE
        // the plugin is loaded/adopts it below. Uses the SAME GPU device +
        // ShaderLibrary the editor ImGui layer was built from. (Create leaves the
        // current ImGui context null; harmless -- the editor's ImGuiLayer re-pins
        // its own context on every BeginFrame/WantCapture*.)
        m_gameImgui = Arcane::OffscreenImGuiLayer::Create(m_gpu->Device(), m_gpu->Shaders());
        if (!m_gameImgui)
        {
            ARC_ERROR("Arcane Editor: OffscreenImGuiLayer creation failed");
            return false;
        }

        // ABI v2: install the ImGui context + allocators on the Runtime BEFORE the
        // plugin loads. PluginHost::RefreshContext copies these into the EngineContext
        // at Init time, so the plugin's Init adopts THIS GImGui across the DLL boundary.
        // The context is the GAME context (not the editor's), so the plugin's DrawUI
        // renders into the viewport pass in MainLoop.
        {
            // Same process-global ImGui allocators the editor context uses (ImGui's
            // allocator functions are global, not per-context); only the context
            // pointer differs from the editor redirect.
            ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
            ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
            m_runtime->SetImGui(m_gameImgui->Context(),
                                reinterpret_cast<void*>(allocFn),
                                reinterpret_cast<void*>(freeFn),
                                ud);
        }

        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        m_plugin.emplace(*m_runtime, std::filesystem::path(gameModule));
        if (!m_plugin->Load())
        {
            ARC_ERROR("Arcane Editor: failed to load game module '{}'", gameModule);
            return false;
        }

        // Task 8: Arcane Editor boots in Edit mode -- the sim starts paused. Play (m_play)
        // unpauses it; Stop restores the snapshot and re-pauses.
        m_runtime->Loop().SetPaused(true);

        // Scene-in-a-panel viewport: an OffscreenCanvas running the SAME
        // canvas->batcher->tonemap path Loom drives, into a panel texture instead
        // of the backbuffer. The device is up by here in both the interactive host
        // and a headless `--frames N` run (which only differs in the audio backend).
        m_viewport = Arcane::OffscreenCanvas::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(), 1280, 720);
        if (!m_viewport)
        {
            ARC_ERROR("Arcane Editor: OffscreenCanvas create failed");
            return false;
        }

        // GPU hit-proxy picker, sized to match the viewport (resized together).
        // Supersampled 2x: the id target feeds the JFA outline below, which needs
        // sub-pixel silhouette coverage to seed a smooth distance field.
        m_pick = Arcane::PickBuffer::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                            1280, 720, /*supersample*/ 2);
        if (!m_pick)
        {
            ARC_ERROR("Arcane Editor: PickBuffer create failed");
            return false;
        }

        // Selection + hover outline (Edit-mode viewport pass), a sibling of
        // m_pick: created and resized at the same viewport size (its own targets
        // are 1x -- it derives the id buffer's supersample factor internally).
        m_outline = Arcane::SelectionOutline::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                                     1280, 720);
        if (!m_outline)
        {
            ARC_ERROR("Arcane Editor: SelectionOutline creation failed");
            return false;
        }

        // Editor undo/redo history. The resolver re-reads Runtime::Registry()
        // EVERY call rather than capturing a Registry& up front: Runtime swaps
        // out the registry object on Play/Stop (PlaySession -> Runtime::
        // SnapshotRegistry/RestoreRegistry) and on plugin hot-reload, so a
        // cached reference would dangle. `rt` is a raw Runtime* into m_runtime
        // (stable across that swap -- only the registry INSIDE it is replaced),
        // safe because m_undo destructs before m_runtime (declaration order).
        m_undo.emplace([rt = &*m_runtime]() -> Astra::Registry& { return rt->Registry(); });

        return true;
    }

    void EditorApp::InstallConsoleSink()
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& m)
            {
                m_console.Push(std::string(m.payload.data(), m.payload.size()));
            });
        m_consoleSink = cb;
        Arcane::Log::Engine()->sinks().push_back(cb);
    }

    void EditorApp::MainLoop()
    {
        auto simPrev = std::chrono::steady_clock::now();
        auto lastFrameTime = simPrev;
        bool running = true;

        while (running)
        {
            auto events = m_gpu->Win().PumpEvents();
            if (events.quitRequested) break;
            if (events.resized) m_gpu->OnResize(events.width, events.height);
            if (m_gpu->Win().IsMinimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Input sample (before ImGui BeginFrame so capture flags are set).
            // Set inside the block below once inViewport + the game context's
            // last-frame WantCaptureMouse are known; stays in scope past the block
            // so the click-pick further down this same frame can also honor it.
            bool gameUiClaims = false;
            {
                // Cleared here, set below if a gizmo drag starts or ends THIS frame --
                // the click-pick block (later, after ImGui builds the Viewport panel)
                // checks it to avoid treating a gizmo-handle click as a selection change.
                m_gizmoCapturedClick = false;

                const auto now = std::chrono::steady_clock::now();
                const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
                lastFrameTime = now;
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
                // the viewport texture -- see the DrawUI block below) sits over the
                // scene, so a click on one of its widgets must claim the pointer ahead
                // of both the plugin's gameplay input and the editor gizmo/click-pick.
                // Uses the game context's LAST-frame WantCaptureMouse: this frame's
                // game ImGui pass runs later (after this input block closes -- see
                // MainLoop), so its capture state is not known yet this frame. That
                // 1-frame lag matches the one already inherent to inViewport/
                // m_viewportActive (both computed from the previous frame's panel
                // hover/focus).
                gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
                    m_play.IsPlaying(), inViewport, m_gameImgui->WantCaptureMouse());

                // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
                // game ImGui pass, which composites into the viewport AFTER this input
                // block's scope closes (see MainLoop). Only the few values the game
                // context needs are hoisted -- the input block stays narrow. Off the
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
                if (!m_play.IsPlaying() || gameUiClaims)
                    pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);
                m_runtime->SetInputSnapshot(pluginSnap);
                m_gpu->Input().Update(frameDt, snap);

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
                {
                    const bool ctrl  = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);
                    const bool shift = snap.ScancodeDown(kScLShift) || snap.ScancodeDown(kScRShift);
                    const bool undoKeyDown = ctrl && !shift && snap.ScancodeDown(kScZ);
                    const bool redoKeyDown = ctrl && ((shift && snap.ScancodeDown(kScZ)) ||
                                                       (!shift && snap.ScancodeDown(kScY)));

                    const bool active = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;
                    if (active && undoKeyDown && !m_prevUndoKeyDown) m_undo->Undo();
                    if (active && redoKeyDown && !m_prevRedoKeyDown) m_undo->Redo();
                    m_prevUndoKeyDown = undoKeyDown;
                    m_prevRedoKeyDown = redoKeyDown;
                }

                // Gizmo mode keys: W=Translate, E=Rotate, R=Scale (SDL_SCANCODE_W/E/R --
                // no conflict with the Sandbox plugin's camera, which uses RMB-pan +
                // wheel-zoom only). Edge-triggered off the raw hardware snapshot, same
                // pattern as the undo/redo keybinds above. Edit-mode + viewport-focus
                // only, so typing/clicking in another panel never changes the gizmo mode.
                {
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

                // Transform-gizmo interaction: hit-test + drag against the selected
                // entity's LocalTransform, bracketed into the undo stack (one drag =
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
                {
                    const bool lmbDown          = (snap.mouseButtons & 0x1u) != 0;
                    const bool mousePressedLeft  = lmbDown && !m_prevLmbDown;
                    const bool mouseReleasedLeft = !lmbDown && m_prevLmbDown;
                    const glm::vec2 mouseScreen(lx, ly);
                    const bool ctrlHeld = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);

                    // An in-progress drag must keep tracking (and commit on release)
                    // even after the cursor leaves the viewport rect -- only a FRESH
                    // drag requires the cursor in-viewport. Aborting a live drag on
                    // viewport-exit would strand the LocalTransform at a mid-drag value
                    // with no undo record (CommandStack::Cancel does not revert), so
                    // viewport-exit is deliberately NOT an abort. !gameUiClaims is
                    // defensive: gameUiClaims is Play-only and this is already
                    // !IsPlaying()-gated, so it is a no-op today, but it keeps this
                    // gate correct if the gizmo is ever allowed to run in Play.
                    const bool gizmoActive = !m_play.IsPlaying() && !gameUiClaims &&
                                             m_gizmoEnabled && m_selection.HasSelection() &&
                                             (m_gizmoDrag.active || inViewport);
                    Astra::Registry*        regPtr = nullptr;
                    Arcane::LocalTransform* lt     = nullptr;
                    if (gizmoActive)
                    {
                        regPtr = &m_runtime->Registry();
                        lt = regPtr->GetComponent<Arcane::LocalTransform>(m_selection.selected);
                    }

                    if (lt)
                    {
                        const Astra::Entity sel = m_selection.selected;
                        const Arcane::GizmoTransform gt{ lt->position, lt->rotation, lt->scale };
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
                                        FindLocalTransformDescriptor(*regPtr, sel);
                                    if (desc)
                                    {
                                        m_undo->Begin("Gizmo");
                                        m_undo->SnapshotComponent(sel, desc);
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
                            lt->position = nt.position;
                            lt->rotation = nt.rotation;
                            lt->scale    = nt.scale;
                            m_gizmoHovered = m_gizmoDrag.axis;   // keep the active handle highlighted

                            if (mouseReleasedLeft)
                            {
                                m_undo->Commit();   // no-move drag self-drops (after == before)
                                m_gizmoDrag          = {};
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
                            m_undo->Cancel();
                            m_gizmoDrag = {};
                        }
                    }

                    m_prevLmbDown = lmbDown;
                }
            }

            // Sim advance through the RunLoop with the plugin callbacks interleaved.
            {
                const auto now = std::chrono::steady_clock::now();
                double simDt = std::chrono::duration<double>(now - simPrev).count();
                simPrev = now;
                if (simDt > 0.25) simDt = 0.25;
                const Arcane::PluginVTable* vt = m_plugin->Vtable();
                m_runtime->Loop().Advance(simDt,
                    [&](double dt)          { if (vt) vt->FixedUpdate(dt); },
                    [&](double dt, double a){ if (vt) vt->Update(dt, a); });
                m_runtime->AudioSystem().Update(simDt);
            }

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

            // Scene -> offscreen canvas (the SAME canvas->batcher->tonemap path Loom
            // drives, but into a panel texture). SetRenderContext writes RenderContext2D
            // in Arcane.dll and applies the plugin's stored camera; SubmitRender runs the
            // render scheduler (sprite submission + physics debug overlay) into this
            // batcher. Runs BEFORE ImGui builds the Viewport panel's Image so the texture
            // is ready when ImGui samples it at backbuffer-render time.
            m_viewport->Draw(
                [&](Arcane::Batcher2D& b)
                {
                    m_runtime->SetRenderContext(&b);
                    m_runtime->Loop().SubmitRender();

                    // Transform gizmo, drawn AFTER the scene submit so it renders on
                    // top. Frame ordering guarantees the input block above already ran
                    // this frame, so m_gizmoHovered/m_gizmoDrag and the live
                    // LocalTransform read here are current. Edit-mode + has-selection
                    // only (mirrors the interaction gate; no viewport-hover requirement
                    // here -- the gizmo should stay visible while e.g. the mouse is over
                    // the Inspector, just not be interactable there).
                    if (!m_play.IsPlaying() && m_gizmoEnabled && m_selection.HasSelection())
                    {
                        Arcane::LocalTransform* lt = m_runtime->Registry().GetComponent<Arcane::LocalTransform>(
                            m_selection.selected);
                        if (lt)
                        {
                            const Arcane::GizmoTransform gt{ lt->position, lt->rotation, lt->scale };
                            const Arcane::GizmoView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                            Arcane::Draw(b, m_gizmoMode, m_gizmoSpace, gt, view, m_gizmoHovered,
                                        m_gizmoDrag.active ? m_gizmoDrag.axis : Arcane::GizmoAxis::None);
                        }
                    }
                },
                glm::vec4(0.02f, 0.02f, 0.04f, 1.0f));

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
            const Arcane::PluginVTable* vtGame = m_plugin->Vtable();
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
                vtGame->DrawUI();
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

            // Selection + hover outline -> the viewport's own layer (Edit only). Refreshes
            // the hit-proxy id buffer, then edge-detects it into the post-tonemap output
            // texture (amber selected, cyan hovered). Skipped entirely when there is nothing
            // to outline. Play mode: not run (the game-imgui overlay owns this slot instead,
            // see above -- the two are mutually exclusive by mode).
            if (!m_play.IsPlaying() && (m_selection.HasSelection() || m_lastInViewport))
            {
                const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                m_pick->RenderIdPass(m_runtime->Registry(), view);

                Arcane::SelectionOutline::Params op;
                op.selectedId = m_selection.HasSelection()
                              ? m_pick->PassIdOf(m_selection.selected) : 0u;
                op.cursorPx   = m_lastInViewport
                              ? glm::ivec2((int)m_lastViewportMouse.x, (int)m_lastViewportMouse.y)
                              : glm::ivec2(-1, -1);

                m_gpu->Cmd()->open();
                m_outline->Render(m_gpu->Cmd(), m_pick->IdTarget(),
                                  m_viewport->OutputFramebuffer(), op);
                m_gpu->Cmd()->close();
                m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            }

            // ImGui: editor shell -- full-viewport dockspace + Sim toolbar + Console panel
            // + the Viewport panel showing the scene texture just rendered above.
            m_gpu->Imgui().BeginFrame();
            Arcane::Editor::BeginDockSpace(*m_undo);
            Arcane::Editor::DrawSimTimeToolbar(m_play, *m_runtime, m_plugin->Vtable());
            Arcane::Editor::EndDockSpace();
            Arcane::Editor::DrawAssetsPanel(m_runtime->CurrentProject());
            Arcane::Editor::DrawConsolePanel(m_console);

            Arcane::Editor::ViewportPanelResult vp =
                Arcane::Editor::DrawViewportPanel(m_viewport->TextureId(),
                                            m_viewport->Width(), m_viewport->Height(),
                                            m_gizmoEnabled, m_gizmoMode, m_gizmoSpace);
            m_pendingViewportW = vp.desiredW;
            m_pendingViewportH = vp.desiredH;
            m_viewportRect     = vp.imageRect;
            m_viewportActive   = Arcane::Editor::SceneInputActive(vp.hovered, vp.focused);

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
            // this frame (gameUiClaims, computed in the input block above) so a click
            // on a game HUD widget does not also re-pick the editor selection.
            if (vp.clicked && !m_gizmoCapturedClick && !m_gizmoDrag.active && !gameUiClaims)
            {
                const Arcane::PickView view{ m_runtime->CameraOffset(),
                                             m_runtime->CameraZoom() };
                const Astra::Entity picked = m_pick->Pick(
                    m_runtime->Registry(), view,
                    glm::vec2(vp.clickLocalX, vp.clickLocalY));
                if (picked.IsValid())
                    m_selection.Select(picked);
                else
                    m_selection.Clear();
            }

            Arcane::Editor::DrawHierarchyPanel(m_runtime->Registry(), m_selection);
            Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo, !m_play.IsPlaying());

            // (The hosted plugin's DrawUI now renders into its OWN ImGui context,
            // composited into the viewport texture above -- not the editor context.)

            nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
            if (!backbuffer) { ImGui::EndFrame(); continue; }

            m_gpu->Cmd()->open();
            // Clear the backbuffer directly (Arcane Editor's scene will live in a panel,
            // so there is no scene->tonemap->backbuffer pass as in Loom).
            m_gpu->Cmd()->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                            nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
            nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_gpu->Cmd()->close();
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();

            m_plugin->Poll();

            ++m_frameCount;
            if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) running = false;
        }
    }

    void EditorApp::Shutdown()
    {
        // Deregister the console sink FIRST, before anything below can log through
        // Arcane::Log::Engine(). m_console is declared before m_runtime/m_plugin/m_gpu
        // in EditorApp.hpp, so it destructs BEFORE them; if the sink outlived this
        // point, a log emitted during ~GpuContext's Vulkan device teardown (validation
        // messages) would invoke the callback and Push into an already-destroyed
        // m_console deque.
        if (m_consoleSink)
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), m_consoleSink), sinks.end());
            m_consoleSink.reset();
        }

        // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
        // the guard covers a future partial-init/destructor path.
        if (m_gpu) m_gpu->Device().Nvrhi()->waitForIdle();
        ARC_INFO("Arcane Editor exiting after {} frames", m_frameCount);

        // The member destructors then run (after Run returns + ~EditorApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped.
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
        //                cache release their NVRHI handles before the device), window
        //                LAST. So gpu outlives runtime + plugin exactly as Loom's did.
        //                See GpuContext's header.
        // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
    }

    int EditorApp::Run()
    {
        if (!Init()) return 1;
        MainLoop();
        Shutdown();
        return 0;
    }
}
