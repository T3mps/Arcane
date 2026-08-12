// RuntimeApp: Init -> MainLoop -> Shutdown. The frame loop is the M4 Playground loop
// (scene content removed) interleaving the plugin's FixedUpdate/Update via the
// RunLoop with the engine schedulers, plus a PluginHost watching the game DLL.
// The render plumbing + teardown order live in GpuContext (m_gpu). The teardown
// CONTRACT is encoded in the RuntimeApp member declaration order -- see RuntimeApp.hpp.

#include "RuntimeApp.hpp"

#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Assets/Assets.hpp>      // Arcane::SaveTexturePng (--screenshot)
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat -- the hang watchdog's liveness signal
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>          // Arcane::Guid::FromString (--scene override; not pulled in transitively by any of the below)
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Batcher2D.hpp>   // Arcane::Batch2DStats (loop HUD + perf tick)
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/FullscreenMaterialChain.hpp>   // scene post hook
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuPassScope -- the F-8b pass seams
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (the scene owns the view)

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <thread>

RuntimeApp::RuntimeApp(Arcane::HostConfig cfg, Arcane::BootSplashWindow* splash)
    : m_config(std::move(cfg)), m_perf(m_config.perf), m_splash(splash),
      m_splashPresenter(m_splash) {}

// ---- Boot stages (Task 8: RuntimeApp::Init folded into RuntimeStages) ----
// Each method below is one block lifted out of the old monolithic Init().
// Run() wires each into the matching id on the vector
// Arcane::HostBoot::RuntimeStages(ctx) returns -- see ProjectBoot.cpp's
// CoreStages header comment for why the body has to live here (host-owned
// members) rather than in Arcane.dll.

bool RuntimeApp::StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx)
{
    // The TypeContext is the process-wide type-identity singleton shared across
    // ArcaneRuntime.exe, Arcane.dll, and every loaded plugin. It is intentionally
    // heap-allocated and never freed: TypeMeta entries registered by the plugin
    // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
    // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
    // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
    // ever destructed, ~std::function() would invoke those thunks -> crash.
    // Heap-leaking is the correct production pattern for a long-running host;
    // the OS reclaims all process memory on exit anyway.
    // (The test exe is safe for a different reason: MetaRegistry::Register is first-writer-wins,
    // and the test TUs that reflect these types are never unloaded, so their thunks -- not the
    // plugin's -- own the MetaRegistry entries.)
    m_typeContext = new Astra::TypeContext();
    // Install the shared context in THIS module too. Astra's
    // GetTypeContext()/SetTypeContext() resolve through a PER-MODULE static slot by
    // design, and Runtime's ctor only installs it for Arcane.dll's slot
    // (Runtime.cpp:120) -- ArcaneRuntime.exe is a separate binary and needs its own
    // install, exactly as ArcaneEditor.exe does (EditorApp.cpp).
    //
    // THIS LINE'S ABSENCE WAS A REAL BUG (2026-07-30): the exe compiles the
    // header-only ActiveSceneCamera sweep, so its TypeID<Camera>::Value() resolved
    // against this module's own EMPTY DefaultTypeContext() and got an id that
    // aliased Transform's in the shared one. CreateView<Camera> then returned every
    // entity that had a Transform and read those bytes as a Camera -- position.x
    // landed in orthographicSize, read 0, so the sweep reported "no usable camera",
    // the view stayed identity at 1 px per metre, and a 1 m sprite drew as a
    // single pixel in the corner. It looked exactly like "the sprite is missing".
    // Nothing caught it because until the camera became a scene component this exe
    // never touched a component type from its OWN code -- it only drove the plugin.
    // Task 8 folds the fix into the SHARED type_context_install stage
    // (ProjectBoot.cpp), which VerifySharedTypeContext's every host that
    // populates ctx.runtime -- this exe's own SetTypeContext call still has to
    // happen HERE, in this module, because Astra's slot is per-module.
    Astra::SetTypeContext(m_typeContext);
    // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
    // until quit). The scripted "ArcaneRuntime --frames N" GPU-verify is headless -> false ->
    // miniaudio's null backend (no real device grabbed on a CI box).
    m_runtime.emplace(m_typeContext, m_config.maxFrames == 0);

    // Populate ctx for the SHARED type_context_install / project_open /
    // input_config stage bodies (ProjectBoot.cpp), which only have `ctx`, not
    // `this` -- "stages populate as they go".
    ctx.runtime = &*m_runtime;
    return true;
}

bool RuntimeApp::StageGpuCore(Arcane::HostBoot::BootContext& ctx)
{
    // The whole platform/render/input stack, booted in order. Owned by m_gpu and
    // declared BEFORE m_runtime/m_plugin in RuntimeApp -- so it destructs AFTER them:
    // the render resources it owns (window/device/swapchain/shaders/canvas/batcher/
    // tonemap/imgui/input + commandList/framebuffers) must outlive runtime + plugin.
    m_gpu = Arcane::GpuContext::Create(m_config);
    if (!m_gpu)
    {
        ARC_ERROR("ArcaneRuntime: GPU context create failed");
        return false;
    }

    ARC_INFO("{} -- ArcaneRuntime host, backend {}", Arcane::BuildInfo(), Arcane::ToString(m_config.backend));
    ctx.gpu = m_gpu.get();

    // Does NOT construct m_presenter here (Task 8c, 2026-07-30 correction):
    // the swapchain-backed BootPresenter is needed exactly once now, for the
    // single reveal frame StageFinalize draws right before Show()/Close() --
    // BootSequence's per-stage pump is driven by the pre-device splash
    // (Arcane::BootSplashPresenter, bound for the whole Run() call in Run()
    // below) for the ENTIRE boot instead. m_presenter is emplaced lazily
    // inside StageFinalize, the one place it is used.
    return true;
}

bool RuntimeApp::StageRenderBridge(Arcane::HostBoot::BootContext&)
{
    // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
    // Runtime so a plugin can build its own engine render objects (e.g. the
    // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
    // plugin (m_gpu is declared before the runtime/plugin in RuntimeApp). Null in a
    // headless host -> the plugin skips its GPU-resource creation.
    m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

    // ABI v2: install the host's ImGui context + allocators on the Runtime BEFORE
    // the plugin loads. PluginHost::RefreshContext copies these into the EngineContext
    // at Init time, so the plugin's Init adopts the host's GImGui across the DLL boundary.
    // The ImGuiLayer (in m_gpu) has already created + set the current context by here.
    {
        ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
        ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
        m_runtime->SetImGui(ImGui::GetCurrentContext(),
                            reinterpret_cast<void*>(allocFn),
                            reinterpret_cast<void*>(freeFn),
                            ud);
    }

    // Window reveal used to fold in HERE (Show() then Close() the splash),
    // right after gpu_core, matching the pre-Task-8c design where the editor
    // window carried the loading bar. Task 8c (2026-07-30 correction) moves
    // that to StageFinalize instead -- see that method's comment for the
    // reveal-ordering reasoning, which now needs to run dead last, not right
    // after gpu_core.
    return true;
}

bool RuntimeApp::StagePluginLoad(Arcane::HostBoot::BootContext&)
{
    // The runtime hosts whatever the project's manifest names (--plugin overrides,
    // for bare-DLL workflows). NOTHING to host is a refusal, not an empty window:
    // the editor is a workshop and meaningfully opens project-less, but this
    // host's one job is running a game. (Until 2026-08-11 a bare run fell back
    // to the retired physics Sandbox showcase; the in-repo module is now
    // ReferenceProject's -- `ArcaneRuntime --project ReferenceProject`.)
    const std::string gameModule =
        Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
    // Secondary plugins: each enabled project plugin that built a Plugins/<name>/Binaries
    // DLL loads alongside the game module through the same host (content-only plugins are
    // already mounted at Project::Open).
    const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
    if (gameModule.empty() && pluginModules.empty())
    {
        ARC_ERROR("ArcaneRuntime: nothing to host -- pass --project <dir> (a project whose "
                  "manifest names a gameModule) or --plugin <dll>");
        return false;
    }
    m_plugin.emplace(*m_runtime, gameModule.empty() ? std::filesystem::path{}
                                                    : std::filesystem::path(gameModule));
    for (const auto& dll : pluginModules)
        m_plugin->AddPlugin(dll);
    if (!m_plugin->Load())
    {
        ARC_ERROR("ArcaneRuntime: failed to load game module '{}'", gameModule);
        return false;
    }

    // Open into the project's boot scene, so `--project X` shows the SAME world
    // the editor shows for X instead of only whatever the plugin's Init spawned.
    // After the plugin load for the same reason EditorApp does it there: a
    // scene naming a component the game module registers would otherwise be
    // silently dropped -- which is why this stays IN StagePluginLoad rather than
    // moving to StageSpriteTables: sprite_tables and plugin_load are DAG
    // SIBLINGS (both depend only on {project_open, render_bridge}), so nothing
    // stops sprite_tables from running first; keeping the boot-scene load
    // sequenced right after this stage's own Load() call is what preserves
    // the "after plugin load" ordering the original code required. The result
    // is discarded -- ArcaneRuntime has no scene session to adopt it into, and
    // BootScene already logs both the file it loaded and every reason it did
    // not, so a project with no/broken boot scene just keeps what the plugin
    // built rather than failing the host. No --project means no project,
    // hence no call.
    if (const Arcane::Project* proj = m_runtime->CurrentProject())
    {
        // --scene overrides the project's manifest bootScene with an explicit
        // asset Guid (HostConfig::sceneOverride) -- the editor's separate-window
        // Play passes the currently-open scene here so ArcaneRuntime boots the SAME scene
        // instead of whatever the manifest names. Invalid override TEXT fails
        // this stage outright (a typo'd --scene is a launch mistake, not the
        // normal "no boot scene" case); a well-formed Guid that resolves to no
        // asset in this project falls through to BootScene's existing
        // missing-bootScene path unchanged (logged there; the host keeps
        // whatever the plugin built).
        if (!m_config.sceneOverride.empty())
        {
            const std::optional<Arcane::Guid> ov = Arcane::Guid::FromString(m_config.sceneOverride);
            if (!ov || !ov->IsValid())
            {
                ARC_ERROR("ArcaneRuntime: --scene '{}' is not a valid asset id", m_config.sceneOverride);
                return false;
            }
            (void)Arcane::HostBoot::BootScene(*m_runtime, *proj, *ov);
        }
        else
        {
            (void)Arcane::HostBoot::BootScene(*m_runtime, *proj);
        }
    }
    else if (!m_config.sceneOverride.empty())
    {
        // --scene names an asset IN a project, so with no project open (absent or
        // failed --project) there is nothing to resolve it against. Say so rather
        // than dropping it silently -- a silent no-op here looks identical to a
        // scene that booted.
        ARC_WARN("ArcaneRuntime: --scene '{}' ignored -- no project is open",
                 m_config.sceneOverride);
    }
    return true;
}

bool RuntimeApp::StageSpriteTables(Arcane::HostBoot::BootContext&)
{
    // Scene asset resolution (sprite-resolution lift): the sprites, sprite
    // materials and post chain a scene REFERENCES become bindable here.
    // Refresh sweeps the live registry every frame, so a scene loaded (or
    // hot-reloaded) later is picked up all the same regardless of the exact
    // ordering between this stage and StagePluginLoad's boot-scene load
    // (they are DAG siblings -- see StagePluginLoad's comment).
    //
    // The compile service is the same one the editor runs: material assets are
    // HLSL that gets stitched and compiled on demand, and ArcaneRuntime's
    // postbuild already ships dxcompiler.dll + dxil.dll + shaders/materials
    // beside the exe for exactly this (premake5.lua). A missing DXC degrades to
    // a warn: sprites still resolve (no compile step), materials and the post
    // chain simply stay unbound.
    if (!m_shaderCompiler.Initialize(/*debounceSeconds=*/0.2))
        ARC_WARN("ArcaneRuntime: dxcompiler.dll unavailable -- sprite materials "
                 "and the scene post chain will not bind");
    m_shaderSources.AddRoot("data/shaders");

    Arcane::SceneRenderResolver::Services rs;
    rs.runtime  = &*m_runtime;
    rs.batcher  = &m_gpu->Batch();
    rs.device   = m_gpu->Device().Nvrhi();
    rs.backend  = m_gpu->Device().Backend();
    rs.compiler = &m_shaderCompiler;
    rs.sources  = &m_shaderSources;
    // No consumeFirst: a standalone host has no open documents to give first
    // refusal to, so every drained result goes straight to the caches.
    m_resolver.emplace(std::move(rs));
    return true;
}

bool RuntimeApp::StageFinalize(Arcane::HostBoot::BootContext&)
{
    // Task 8c (2026-07-30 correction, "the splash carries the loading UI,
    // not the editor window"): the window reveal moved here from
    // StageRenderBridge (see that method's comment) -- RuntimeStages appends
    // nothing (BootStageParityTest pins that), so there is no dedicated
    // splash_ready id to hang this on the way the editor has one; "finalize"
    // is the LAST core stage both hosts run, matching where the editor's own
    // splash_ready now sits (it depends on "finalize" -- see ProjectBoot.cpp).
    //
    // Same two constraints as the editor's StageSplashReady, and the same
    // resolution -- see that method's comment in EditorApp.cpp for the full
    // reasoning (UnrealEdGlobals.cpp:215-236's hide-then-show shape):
    //   1. Never reveal an undrawn window: Present() one real frame through
    //      the swapchain-backed BootPresenter (fraction=1.0, the terminal
    //      tick) before doing anything else -- nothing has drawn into this
    //      window's swapchain yet, because BootSequence's per-stage pump has
    //      been driven by the pre-device splash presenter for the WHOLE run.
    //   2. Never leave a gap with neither window on screen: Show() the real
    //      window (now holding that frame) BEFORE Close()ing the splash.
    //
    // 2026-07-30 review round 2, finding 2's second half: see EditorApp::
    // StageSplashReady's matching comment for why Present()'s return alone
    // is not enough (it returns true on BOTH "drew and presented" and "no
    // backbuffer this call, drew nothing"), and why one retry + a hard
    // refusal to Show() an undrawn window follow.
    m_presenter.emplace(*m_gpu, Arcane::BootPresenterMode::Fullscreen);
    Arcane::BootProgress done;   // stageId/detail empty: the terminal tick
    done.fraction = 1.0f;

    bool ok = m_presenter->Present(done);
    if (ok && !m_presenter->HasPresentedFrame())
        ok = m_presenter->Present(done);   // one retry: transient no-backbuffer (zero-size/out-of-date)
    if (!ok)
    {
        // Quit requested during this pump (m_gpu's own window's event
        // backlog, unpumped for the whole boot until this exact call --
        // distinct from m_splashPresenter's quit detection, which only
        // covers the splash). Same honest asymmetry as the editor: this
        // path reports exit code 1, not the 0 BootResult::quitRequested
        // would give, because that flag is set only by BootSequence::Run's
        // OWN present() calls, not a stage's return value.
        return false;
    }
    if (!m_presenter->HasPresentedFrame())
    {
        ARC_ERROR("ArcaneRuntime: failed to present the boot-complete frame -- refusing to reveal an undrawn window");
        return false;
    }

    // Show() also RAISES (Task 8d defect B) -- and must run while the splash
    // still holds the foreground, strictly before the Close() below. Same
    // constraint and same reason as EditorApp::StageSplashReady.
    m_gpu->Win().Show();
    // Disarm BEFORE Close() -- see EditorApp::StageSplashReady's matching
    // comment for why this specific ordering is required.
    m_splashPresenter.Disarm();
    if (m_splash) m_splash->Close();
    return true;
}

void RuntimeApp::MainLoop()
{
    // The reused command list + the lazy backbuffer-framebuffer cache live in
    // m_gpu (m_gpu->Cmd() / m_gpu->FramebufferFor(bb)) so they release their
    // NVRHI handles before the device, in m_gpu's teardown.

    auto simPrev       = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    auto lastShaderPoll = simPrev;
    bool running = true;

    // Boot is over; anything the watchdog reports from here on belongs to the
    // frame loop, not to a stale boot stage.
    Arcane::Diagnostics::SetPhase("runtime frame loop");

    while (running)
    {
        // FIRST statement in the frame, before anything can block. Mirrors
        // EditorApp::MainLoop -- the two hosts must not diverge on liveness.
        Arcane::Diagnostics::Heartbeat();

        auto events = m_gpu->Win().PumpEvents();
        if (events.quitRequested) break;
        if (events.resized)
            m_gpu->OnResize(events.width, events.height);
        if (m_gpu->Win().IsMinimized())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        m_perf.FrameStart();

        // Input: sample SDL state, evaluate actions. Must precede ImGui BeginFrame
        // so capture flags are set before the evaluator reads them.
        {
            const auto now = std::chrono::steady_clock::now();
            const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
            lastFrameTime = now;
            // The host clock the compile service debounces against + the frame dt
            // the material globals report. Advanced exactly once per frame, here,
            // because this is where the frame's wall-clock delta is measured (the
            // editor's m_editorClock does the same job in its phase 9).
            m_lastFrameDt = frameDt;
            m_hostClock += frameDt;
            const Arcane::InputSnapshot snap =
                m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                          m_gpu->Imgui().WantCaptureMouse());
            m_runtime->SetInputSnapshot(snap);   // plugins read it via Runtime::Input()
            m_gpu->Input().Update(frameDt, snap);
            if (m_gpu->Input().Pressed("quit"))                break;
            if (m_gpu->Input().Pressed("reload_plugin"))       m_plugin->ForceReload();
            if (m_gpu->Input().Pressed("reload_plugin_fresh")) m_plugin->ReloadFresh();
        }

        // Sim advance: clamp dt, drive RunLoop with plugin callbacks interleaved.
        {
            const auto now = std::chrono::steady_clock::now();
            double simDt = std::chrono::duration<double>(now - simPrev).count();
            simPrev = now;
            if (simDt > 0.25) simDt = 0.25;
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_runtime->Loop().Advance(simDt,
                [&](double dt)          { m_plugin->FixedUpdateAll(dt); },
                [&](double dt, double a){ m_plugin->UpdateAll(dt, a); });
            // Reclaim finished fire-and-forget SFX voices each frame (and, on the
            // headless null backend, advance audio time so one-shots actually end).
            m_runtime->AudioSystem().Update(simDt);
            m_perf.Add(m_perf.accSim, t0, m_perf.Now());
        }

        m_gpu->Imgui().BeginFrame();
        {
            ImGui::Begin("ArcaneRuntime");
            ImGui::Text("Backend: %s", Arcane::ToString(m_gpu->Device().Backend()));
            ImGui::Text("Plugin gen: %u", m_plugin->Generation());
            const Arcane::Batch2DStats s = m_gpu->Batch().Stats();
            ImGui::Text("Quads: %u  Draws: %u", s.quads, s.drawCalls);
            ImGui::End();
        }

        // ABI v2: the game module + any secondary plugins draw their own ImGui between
        // BeginFrame and Render. Each entry point is null-checked inside DrawUIAll.
        m_plugin->DrawUIAll();

        nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
        if (!backbuffer)
        {
            // No backbuffer this frame: still balance BeginFrame with EndFrame
            // so ImGui's assert (double-Begin) doesn't fire next iteration.
            ImGui::EndFrame();
            continue;
        }

        // Hot reload: poll shaders once a second; pipeline caches rebuild lazily.
        {
            const auto now0 = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now0 - lastShaderPoll).count() >= 1.0)
            {
                m_gpu->Shaders().Poll();
                lastShaderPoll = now0;
            }
        }

        // Scene asset resolution, BEFORE the batcher's Begin and before
        // SubmitRender: the drain inside registers compiled materials with the
        // batcher (a pipeline/binding-set table mutation that does not belong
        // inside a recording), and the SpriteTable/SpriteMaterialTable it
        // publishes are what THIS frame's submission reads. Without this call the
        // host published no tables at all, so every sprite fell back to the 1x1 m
        // untextured quad and every material to the plain pipeline -- which is
        // exactly what "the game window draws nothing but the editor viewport
        // looks right" was.
        if (m_resolver)
        {
            Arcane::SceneRenderResolver::FrameInfo frame;
            frame.now            = m_hostClock;
            frame.dt             = m_lastFrameDt;
            frame.viewportWidth  = (float)m_gpu->Cnv().Width();
            frame.viewportHeight = (float)m_gpu->Cnv().Height();
            m_resolver->Refresh(frame);
            m_frameGlobals = m_resolver->Globals();
        }

        m_gpu->Cmd()->open();
        // F-8b: the runtime records its WHOLE frame into one command list, so
        // the outer scope is that recording -- everything below nests inside it,
        // and the canvas clear (which has no scope of its own) is covered by it.
        // Held in an optional because the scope must END while the list is still
        // open, and close() is ~100 lines down at the bottom of this loop body:
        // a plain block would mean reindenting the entire frame for one marker.
        std::optional<Arcane::GpuPassScope> framePass;
        framePass.emplace(m_gpu->Cmd(), "pass:frame");

        m_gpu->Cmd()->clearTextureFloat(m_gpu->Cnv().Texture(), nvrhi::AllSubresources,
                                        nvrhi::Color(0.02f, 0.02f, 0.04f, 1.0f));

        m_gpu->Batch().Begin(m_gpu->Cmd(), m_gpu->Cnv().Framebuffer(),
                             m_gpu->Cnv().Width(), m_gpu->Cnv().Height());
        // Engine-global material constants (Time/Delta/Viewport) for registered
        // sprite materials; sticky, so once per frame after Begin. Built-in
        // pipelines ignore them -- but before this arc an animated material read
        // zeros here, because the host never called SetGlobals at all.
        m_gpu->Batch().SetGlobals(m_frameGlobals);

        // Set the render context IN Arcane.dll so TypeID<RenderContext2D> resolves
        // in the correct module; then drive the plugin's RenderSubmissionSystem.
        // ArcaneRuntime stays camera-agnostic: SetRenderContext writes the STORED camera the
        // plugin drives via Runtime::SetCamera (default identity if it never does).
        // Scene camera: the ACTIVE Camera entity owns the view. Pushed HERE, after
        // the plugin's update ran, so a scene that ships a camera beats a plugin
        // that also pushes one -- the scene is the authored artifact. No camera
        // leaves the stored camera untouched (a plugin that drives the camera
        // itself via Runtime::SetCamera therefore still works) and says so
        // once, rather than substituting an identity view that would render an
        // older scene as an unexplained black window.
        {
            int camCount = 0;
            const auto view = Arcane::ActiveSceneCamera(m_runtime->Registry(),
                                                        (float)m_gpu->Cnv().Width(),
                                                        (float)m_gpu->Cnv().Height(),
                                                        &camCount);
            if (view)
                m_runtime->SetCamera(view->offset, view->zoom);
            else if (!m_warnedNoSceneCamera)
            {
                // The old message said "no active Camera entity" for THREE
                // different situations and cost a debugging round: no Camera
                // component in the scene at all, one present but active==false or
                // orthographicSize<=0, or a component view that is empty in THIS
                // module because it disagrees with the loader about the component
                // id. Count the components here, host-side, and say which it is --
                // the resolver logs its own census from inside Arcane.dll, so the
                // two lines together localise the last case.
                int total = 0;
                m_runtime->Registry().CreateView<Arcane::Camera>().ForEach(
                    [&](Astra::Entity, Arcane::Camera&) { ++total; });
                if (total == 0)
                    ARC_WARN("scene has no Camera component at all -- nothing sets the view. "
                             "Add a Camera component to an entity (a New Scene ships one).");
                else
                    ARC_WARN("scene carries {} Camera component(s) but none is usable "
                             "(active == false, or orthographicSize <= 0) -- nothing sets "
                             "the view", total);
                m_warnedNoSceneCamera = true;
            }
            if (camCount > 1 && !m_warnedMultiSceneCamera)
            {
                ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
                m_warnedMultiSceneCamera = true;
            }
        }

        {
            // Scope names deliberately mirror FramePerf's acc* accumulators
            // (F-8b): a CPU timeline and a GPU timeline that use two vocabularies
            // for the same phases cannot be read side by side.
            Arcane::GpuPassScope pass(m_gpu->Cmd(), "pass:rec");
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_runtime->SetRenderContext(&m_gpu->Batch());
            m_runtime->Loop().SubmitRender();
            m_perf.Add(m_perf.accRec, t0, m_perf.Now());
        }

        {
            // Where the batcher's draws actually get recorded -- Begin() above
            // only sets state, End() is the flush.
            Arcane::GpuPassScope pass(m_gpu->Cmd(), "pass:end");
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Batch().End();
            m_perf.Add(m_perf.accEnd, t0, m_perf.Now());
        }

        nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
        {
            Arcane::GpuPassScope pass(m_gpu->Cmd(), "pass:tone");
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            // Scene post hook (post arc): with a bound chain the linear canvas
            // feeds it as the external Scene input and the tonemap samples the
            // chain's output -- without one this is exactly the old line
            // (byte-identical). The chain comes from the resolver's PostProcess
            // sweep, re-read every frame because a drain may swap the bound
            // instance under an asset re-save.
            Arcane::FullscreenMaterialChain* postChain =
                m_resolver ? m_resolver->PostChain() : nullptr;
            const Arcane::MaterialInstance* postInstance =
                m_resolver ? m_resolver->PostInstance() : nullptr;
            Arcane::Canvas* post =
                (postChain && postChain->Ready() && postInstance)
                    ? m_gpu->EnsurePost() : nullptr;
            if (post)
            {
                postChain->Render(m_gpu->Cmd(), post->Framebuffer(),
                                  *postInstance, m_frameGlobals,
                                  &m_runtime->AssetsFacade(),
                                  static_cast<std::size_t>(-1),
                                  m_gpu->Cnv().Texture());
                m_gpu->Tone().Run(m_gpu->Cmd(), post->Texture(), fb);
            }
            else
            {
                m_gpu->Tone().Run(m_gpu->Cmd(), m_gpu->Cnv().Texture(), fb);
            }
            m_perf.Add(m_perf.accTone, t0, m_perf.Now());
        }
        {
            Arcane::GpuPassScope pass(m_gpu->Cmd(), "pass:imgui");
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_perf.Add(m_perf.accImgui, t0, m_perf.Now());
        }

        // Ends the frame scope BEFORE close(): a marker recorded into a closed
        // list latches the D3D12 marker layer off for the rest of the process
        // and is an access violation on Vulkan.
        framePass.reset();
        m_gpu->Cmd()->close();
        {
            // No pass scope here: submit + present happen with the list already
            // closed, so there is nothing left to record markers into. F-8b's
            // "close + submit + present" row is a CPU seam only.
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();
            m_perf.Add(m_perf.accPresent, t0, m_perf.Now());
        }

        // GPU-progress heartbeat (Task 7): after the frame's last submit.
        m_gpu->FrameProgress().EndFrame();

        // Debounced watcher: rebuild PlaygroundGame -> auto hot reload.
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_plugin->Poll();
            m_perf.Add(m_perf.accPoll, t0, m_perf.Now());
        }

        if (m_perf.On())
        {
            const Arcane::Batch2DStats bs = m_gpu->Batch().Stats();
            m_perf.Tick(bs.quads, bs.drawCalls);
        }

        ++m_frameCount;
        const bool lastFrame = m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames;

        // --screenshot: capture the BACKBUFFER we just presented -- post-tonemap,
        // post-ImGui, i.e. exactly the pixels a player sees. Taken on the final frame
        // only, and SaveTexturePng stalls the device (staging copy + waitForIdle), so
        // it never touches a steady-state frame. After Present, so the capture cannot
        // race the recording that produced it.
        if (lastFrame && !m_config.screenshotPath.empty())
        {
            if (Arcane::SaveTexturePng(m_gpu->Device().Nvrhi(), backbuffer,
                                       m_config.screenshotPath))
                ARC_INFO("screenshot written: {}", m_config.screenshotPath);
            else
                ARC_WARN("screenshot FAILED: {}", m_config.screenshotPath);
        }

        if (lastFrame)
            running = false;
    }
}

void RuntimeApp::Shutdown()
{
    // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
    // the guard covers a future partial-init/destructor path.
    if (m_gpu) m_gpu->Device().Nvrhi()->waitForIdle();
    ARC_INFO("ArcaneRuntime exiting after {} frames", m_frameCount);

    // The member destructors then run (after Run returns + ~RuntimeApp), in reverse
    // declaration order -- the load-bearing TEARDOWN CONTRACT:
    //   m_resolver -> ~SceneRenderResolver: un-publishes the registry's sprite
    //                tables (whose pointers are non-owning) and drops its nvrhi
    //                keep-alive texture handles. Declared LAST so it runs FIRST,
    //                while the runtime it publishes through and the device those
    //                handles belong to are both still alive.
    //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
    //                ResetRegistry) while the plugin DLL is STILL mapped.
    //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
    //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
    //                cache release their NVRHI handles before the device), window
    //                LAST. So gpu outlives runtime + plugin exactly as the old
    //                outer/inner main scopes did. See GpuContext's header.
    // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
}

int RuntimeApp::Run()
{
    Arcane::HostBoot::BootContext ctx{};
    ctx.runtime     = nullptr;              // stages populate as they go
    ctx.gpu         = nullptr;
    ctx.splash      = m_splash;
    ctx.projectPath = m_config.projectPath.c_str();
    ctx.pluginPath  = m_config.pluginPath.c_str();
    ctx.moduleName  = "ArcaneRuntime.exe";

    // Spec sec 6 default: the runtime host shows no boot progress until an
    // opened project's own manifest opts in (project_open's ProjectBoot.cpp
    // override flips this once that manifest is known -- see its own
    // comment). Set here, BEFORE BootSequence::Run is even constructed below,
    // so not even the very FIRST present() call (which fires as soon as
    // "runtime_create" completes, before project_open's Worker body has had
    // any chance to run at all -- BootSequence.cpp's dispatch order) can show
    // progress a player never asked to see. BootSplashWindow::SetShowProgress
    // tolerates m_splash == nullptr, same never-fail contract as every other
    // splash call.
    if (m_splash) m_splash->SetShowProgress(false);

    // HostBoot::RuntimeStages(ctx) is the SAME shared function
    // BootStageParityTest exercises and EditorApp calls for its own list
    // (EditorApp::Run) -- this is the literal call that keeps the two hosts
    // from silently diverging on which steps exist. RuntimeStages already
    // carries the Fatal-ABI-refusal override for project_open (ProjectBoot.cpp),
    // so nothing about that stage needs patching here. Everything else that
    // needs RuntimeApp's own private members (m_gpu/m_runtime/m_plugin/
    // m_resolver/...) gets its `run` overwritten below -- ids/deps/policy/
    // thread/weight (the DAG shape the parity test polices) are untouched.
    std::vector<Arcane::BootStage> stages = Arcane::HostBoot::RuntimeStages(ctx);
    for (Arcane::BootStage& stage : stages)
    {
        if (stage.id == "runtime_create")       stage.run = [this, &ctx] { return StageRuntimeCreate(ctx); };
        else if (stage.id == "gpu_core")         stage.run = [this, &ctx] { return StageGpuCore(ctx); };
        else if (stage.id == "render_bridge")    stage.run = [this, &ctx] { return StageRenderBridge(ctx); };
        else if (stage.id == "sprite_tables")    stage.run = [this, &ctx] { return StageSpriteTables(ctx); };
        else if (stage.id == "plugin_load")      stage.run = [this, &ctx] { return StagePluginLoad(ctx); };
        // "finalize" USED to be DELIBERATELY left as an explicit no-op --
        // RuntimeApp had no window title to recompute and no scene session
        // to adopt (editor-only concepts). Task 8c (2026-07-30 correction)
        // gives it real work: the window reveal, which used to live in
        // StageRenderBridge -- see StageFinalize's own comment for why it
        // moved here specifically (RuntimeStages appends nothing, so
        // "finalize", the last core stage, is where the editor's own
        // splash_ready now points too).
        else if (stage.id == "finalize")         stage.run = [this, &ctx] { return StageFinalize(ctx); };
        // "edit_core" (2026-07-30 review, Fix 5): the editor's undo/redo
        // history and structural-edit binding have no runtime analog -- the
        // runtime has no scene session to undo/redo against. Explicit
        // no-op, stated as one rather than relying on the Unpatched sentinel
        // to happen to look like success.
        else if (stage.id == "edit_core")        stage.run = [] { return true; };
    }

    Arcane::BootSequence seq(std::move(stages));
    // m_splashPresenter is BootSequence's presenter for the WHOLE run (Task
    // 8c) -- from runtime_create through finalize, every per-stage present()
    // call reports into m_splash's status text + taskbar progress rather
    // than the swapchain, AND (2026-07-30 review round 2, finding 2)
    // arms/checks the splash's own open/closed state so IBootPresenter's
    // documented quit contract (BootSequence.hpp:65) still fires if the user
    // closes the splash mid-boot -- see BootSplashPresenter::Present's own
    // comment. Safe to run unconditionally: it tolerates m_splash ==
    // nullptr, same never-fail contract as BootSplashWindow itself. It is a
    // class member, not constructed here, so StageFinalize can Disarm() it
    // before closing the splash intentionally -- see that method. The
    // swapchain-backed BootPresenter (m_presenter) is used exactly once,
    // explicitly, inside StageFinalize -- never through this pump.
    const Arcane::BootResult boot = seq.Run(&m_splashPresenter);
    if (!boot.ok)
        return boot.quitRequested ? 0 : 1;

    MainLoop();
    Shutdown();
    return 0;
}
