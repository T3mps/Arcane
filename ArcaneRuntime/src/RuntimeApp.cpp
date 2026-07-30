// RuntimeApp: Init -> MainLoop -> Shutdown. The frame loop is the M4 Playground loop
// (scene content removed) interleaving the plugin's FixedUpdate/Update via the
// RunLoop with the engine schedulers, plus a PluginHost watching the game DLL.
// The render plumbing + teardown order live in GpuContext (m_gpu). The teardown
// CONTRACT is encoded in the RuntimeApp member declaration order -- see RuntimeApp.hpp.

#include "RuntimeApp.hpp"

#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>          // Arcane::Guid::FromString (--scene override; not pulled in transitively by any of the below)
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Batcher2D.hpp>   // Arcane::Batch2DStats (loop HUD + perf tick)
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/FullscreenMaterialChain.hpp>   // scene post hook
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (the scene owns the view)

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

RuntimeApp::RuntimeApp(Arcane::HostConfig cfg)
    : m_config(std::move(cfg)), m_perf(m_config.perf) {}

bool RuntimeApp::Init()
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
    // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
    // until quit). The scripted "ArcaneRuntime --frames N" GPU-verify is headless -> false ->
    // miniaudio's null backend (no real device grabbed on a CI box).
    m_runtime.emplace(m_typeContext, m_config.maxFrames == 0);

    // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
    // Runtime so a plugin can build its own engine render objects (e.g. the
    // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
    // plugin (m_gpu is declared before the runtime/plugin in RuntimeApp). Null in a
    // headless host -> the plugin skips its GPU-resource creation.
    m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

    // Open the project (if any) BEFORE loading input + the game module: both come from
    // the project when one is given. No --project => CurrentProject() stays null and the
    // legacy data/ + --plugin path is used (non-breaking).
    // CONTRACT, deliberately different from the editor's (spec Part B): opening a
    // project here takes NO editor lock and refuses no rivals. The editor may have
    // the same project open -- that is the normal case, since its separate-window
    // Play spawns us on the scene it is editing -- and two runtimes on one project
    // are fine too. We only ever READ the project. (Spec Part B also said we answer
    // no engine probe; that clause was wrong about the pre-fold state. --print-engine-info
    // IS answered, as the pre-fold host already did, because both hosts share one
    // HostConfig -- see main.cpp. Part A is behavior-preserving, so it stays.)
    if (!m_config.projectPath.empty())
    {
        if (!m_runtime->OpenProject(m_config.projectPath))
            ARC_WARN("ArcaneRuntime: --project '{}' failed to open; using data/ + --plugin fallback",
                     m_config.projectPath);
    }
    if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
        ARC_WARN("ArcaneRuntime: input actions failed to load");

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

    // ArcaneRuntime's default game module is the physics Sandbox showcase: with no --project
    // and no --plugin, host Sandbox.dll. ArcaneRuntime IS that showcase (the ArcaneRuntime --frames
    // GPU-verify + CI depend on it), so it keeps the old default -- the editor, which
    // leaves pluginPath empty, is the host that starts with no game (see EditorApp).
    const std::string fallback = m_config.pluginPath.empty() ? "Sandbox.dll" : m_config.pluginPath;
    const std::string gameModule =
        Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), fallback);
    m_plugin.emplace(*m_runtime, std::filesystem::path(gameModule));
    // Secondary plugins: each enabled project plugin that built a Plugins/<name>/Binaries
    // DLL loads alongside the game module through the same host (content-only plugins are
    // already mounted at Project::Open).
    for (const auto& dll : Arcane::HostBoot::PluginModules(m_runtime->CurrentProject()))
        m_plugin->AddPlugin(dll);
    if (!m_plugin->Load())
    {
        ARC_ERROR("ArcaneRuntime: failed to load game module '{}'", gameModule);
        return false;
    }

    // Open into the project's boot scene, so `--project X` shows the SAME world
    // the editor shows for X instead of only whatever the plugin's Init spawned.
    // After the plugin load for the same reason EditorApp::Init does it there: a
    // scene naming a component the game module registers would otherwise be
    // silently dropped. The result is discarded -- ArcaneRuntime has no scene session to
    // adopt it into, and BootScene already logs both the file it loaded and every
    // reason it did not, so a project with no/broken boot scene just keeps what
    // the plugin built rather than failing the host. No --project means no
    // project, hence no call: the scripted Sandbox.dll GPU-verify is untouched.
    if (const Arcane::Project* proj = m_runtime->CurrentProject())
    {
        // --scene overrides the project's manifest bootScene with an explicit
        // asset Guid (HostConfig::sceneOverride) -- the editor's separate-window
        // Play passes the currently-open scene here so ArcaneRuntime boots the SAME scene
        // instead of whatever the manifest names. Invalid override TEXT fails
        // Init outright (a typo'd --scene is a launch mistake, not the normal
        // "no boot scene" case); a well-formed Guid that resolves to no asset in
        // this project falls through to BootScene's existing missing-bootScene
        // path unchanged (logged there; the host keeps whatever the plugin built).
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

    // Scene asset resolution (sprite-resolution lift): the sprites, sprite
    // materials and post chain a scene REFERENCES become bindable here. Built
    // after the scene boot above only for tidiness -- Refresh sweeps the live
    // registry every frame, so a scene loaded (or hot-reloaded) later is picked
    // up all the same.
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
    m_shaderSources.AddRoot("shaders");

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

void RuntimeApp::MainLoop()
{
    // The reused command list + the lazy backbuffer-framebuffer cache live in
    // m_gpu (m_gpu->Cmd() / m_gpu->FramebufferFor(bb)) so they release their
    // NVRHI handles before the device, in m_gpu's teardown.

    auto simPrev       = std::chrono::steady_clock::now();
    auto lastFrameTime = simPrev;
    auto lastShaderPoll = simPrev;
    bool running = true;

    while (running)
    {
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
        // leaves the stored camera untouched (a plugin-driven camera, e.g. the
        // Sandbox showcase, therefore still works exactly as before) and says so
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
                ARC_WARN("scene has no active Camera entity -- nothing sets the view. "
                         "Add a Camera component to an entity (a New Scene ships one).");
                m_warnedNoSceneCamera = true;
            }
            if (camCount > 1 && !m_warnedMultiSceneCamera)
            {
                ARC_WARN("scene carries {} active Camera entities -- the first found wins", camCount);
                m_warnedMultiSceneCamera = true;
            }
        }

        {
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_runtime->SetRenderContext(&m_gpu->Batch());
            m_runtime->Loop().SubmitRender();
            m_perf.Add(m_perf.accRec, t0, m_perf.Now());
        }

        {
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Batch().End();
            m_perf.Add(m_perf.accEnd, t0, m_perf.Now());
        }

        nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
        {
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
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_perf.Add(m_perf.accImgui, t0, m_perf.Now());
        }

        m_gpu->Cmd()->close();
        {
            const auto t0 = m_perf.On() ? m_perf.Now() : Arcane::FramePerf::Clock::time_point{};
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();
            m_perf.Add(m_perf.accPresent, t0, m_perf.Now());
        }

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
        if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames)
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
    if (!Init()) return 1;
    MainLoop();
    Shutdown();
    return 0;
}
