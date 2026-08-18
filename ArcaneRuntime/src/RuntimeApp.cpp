// RuntimeApp: Init -> MainLoop -> Shutdown. The frame loop is the M4 Playground loop
// (scene content removed) interleaving the plugin's FixedUpdate/Update via the
// RunLoop with the engine schedulers, plus a PluginHost watching the game DLL.
// The render plumbing + teardown order live in GpuContext (m_gpu). The teardown
// CONTRACT is encoded in the RuntimeApp member declaration order -- see RuntimeApp.hpp.

#include "RuntimeApp.hpp"
#include "RuntimeFrame.hpp"   // NRI Phase 3, Task 4: MainLoop's frame body

#include <Arcane/Host/GoldenHarness.hpp>  // Arcane::DrainSceneCompiles/kGoldenWarmupTimeoutSeconds (NRI Phase 3, Task 13: shared with ArcaneEditor -- see that header's extraction history)
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Assets/Assets.hpp>      // Arcane::Assets (AssetsFacade().PixelsFor -- the pre-loop SetPixelSupply lambda)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::Heartbeat/SetPhase (pre-loop phase markers)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>          // Arcane::Guid::FromString (--scene override; not pulled in transitively by any of the below)
#include <Arcane/Project/AssetId.hpp>    // Arcane::AssetId::FromGuid (--nri-graph asset resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (StageGpuCore's boot banner)
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved (Run()'s exit-code tail)
#include <Arcane/Render/PickEmit.hpp>    // PickEntityForId (ShutdownGraphPath's --pick-probe report)
#include <Arcane/Scene/SceneCamera.hpp>  // Arcane::ActiveSceneCamera (PushSceneCamera; both render arms call it via FrameIo::app)
// Assets.hpp/GoldenImage.hpp/AudioDevice.hpp/InputActions.hpp/InputSnapshot.hpp/
// Batcher2D.hpp/FullscreenMaterialChain.hpp moved to RuntimeFrame.cpp at NRI
// Phase 3 Task 4: every symbol they were here for (GoldenArtifact, the audio
// voice reap, input sampling, Batch2DStats, the post-chain hook) moved with
// MainLoop's frame body -- see that file.
// NriGraphContext (the graph vehicle) is NOT Dist-guarded here: RuntimeApp.hpp
// holds the member unconditionally, and as of Phase 5a (Task 2b) the CREATION
// is unconditional too, in every configuration -- see that header's comment.

#include <Astra/Core/TypeContext.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// DrainSceneCompiles (the golden warm-up drain) and GoldenArtifact (the
// capture/compare tail RuntimeFrame.cpp's CaptureTail calls) both moved to
// the shared Arcane::Host seam at NRI Phase 3 Task 13
// (Arcane/Host/GoldenHarness.hpp/.cpp) -- ArcaneEditor's own golden mode
// needed the identical machinery, and an anonymous-namespace helper has
// internal linkage: it cannot be called across translation units, let alone
// a second executable. GoldenArtifact made this same hop once before, from
// here to RuntimeFrame.cpp's anonymous namespace at Task 4 -- see that
// file's own history comment. Both moves are VERBATIM; see the task-13
// report for the diff-match.

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
    //
    // THE BOOT-PATH SPLIT (NRI Phase 3, Task 6). As of Phase 5a (Task 2b) the
    // NRI frame graph is the ONLY render path, unconditionally, in every
    // configuration including Dist: NO NVRHI DEVICE IS EVER CREATED. The
    // graph flavor builds the window, a device-less Batcher2D, the graph
    // ImGuiLayer and the input stack, and MainLoop then builds the
    // NriGraphContext that owns the process's ONLY graphics device over that
    // same window. GpuContext::Create's NVRHI arm is unreachable from here
    // now; it stays only until Tasks 4-11 delete NVRHI outright.
    m_gpu = Arcane::GpuContext::CreateForGraph(m_config);
    if (!m_gpu)
    {
        ARC_ERROR("ArcaneRuntime: GPU context create failed");
        return false;
    }

    ARC_INFO("{} -- ArcaneRuntime host, backend {}{}", Arcane::BuildInfo(),
             Arcane::ToString(m_config.backend),
             m_gpu->GraphFlavor() ? " (NRI graph: no NVRHI device in this process)" : "");
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
    //
    // (nullptr, nullptr) IS THE GRAPH-MODE CONTRACT (NRI Phase 3, Task 6, plan
    // reconciliation 7). There is no NVRHI device to hand over, and the Assets
    // facade is deliberately left device-less by it: Assets::PixelsFor (Task 1)
    // is the retained, device-FREE decode the graph's NriTextureCache uploads
    // from, so a textured sprite still renders. What a plugin loses is
    // OffscreenCanvas::Create, which refuses loudly against a null device and
    // names the Phase-5 arc that gives it a graph-side answer.
    if (m_gpu->GraphFlavor())
        m_runtime->SetRenderResources(nullptr, nullptr);
    else
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
    //
    // Debounce is a HOT-RELOAD nicety: a 0.2 s quiet window keeps a designer
    // holding Ctrl+S from firing a compile per keystroke. A golden run has no
    // designer and no re-saves -- there the window is pure latency between "the
    // scene declared a material" and "a frame that gets captured can contain
    // it", and Poll measures it against the PINNED clock (1/60 per frame), so
    // it spends a fixed 12 frames of the run's budget for nothing. Zero it, and
    // let MainLoop's warm-up drain the compiles to quiescence before frame 1
    // rather than racing them (see DrainSceneCompiles).
    const bool golden = m_config.GoldenMode();
    if (!m_shaderCompiler.Initialize(/*debounceSeconds=*/golden ? 0.0 : 0.2))
    {
        // A golden run whose materials CANNOT bind would capture a frame
        // missing half its content and still exit 0 -- and that frame becomes
        // the frozen baseline the rest of the phase is compared against.
        // Refuse the boot instead. An interactive run keeps degrading to a
        // warning, where the missing material is on screen and recoverable.
        if (golden)
        {
            ARC_ERROR("ArcaneRuntime: dxcompiler.dll unavailable -- a golden run cannot bind "
                      "sprite materials or the scene post chain, and would capture or compare "
                      "a frame that is missing them");
            return false;
        }
        ARC_WARN("ArcaneRuntime: dxcompiler.dll unavailable -- sprite materials "
                 "and the scene post chain will not bind");
    }
    m_shaderSources.AddRoot("data/shaders");

    Arcane::SceneRenderResolver::Services rs;
    rs.runtime  = &*m_runtime;
    rs.batcher  = &m_gpu->Batch();
    // Device-less on the graph path, and the caches are built for it (NRI
    // Phase 3, Task 2's severance): SpriteMaterialCache registers BYTES-ONLY
    // materials with the device-less batcher, and PostChainCache publishes its
    // PostChainDesc without building the NVRHI FullscreenMaterialChain. The
    // BACKEND is still needed either way -- it selects the shader flavor the
    // compiles target -- and on the graph path comes from the config, since
    // there is no device to ask. The two values are always equal
    // (GpuContext::Create passes exactly this config field into
    // RenderDeviceDesc::backend); written as a gate so the NVRHI arm keeps the
    // literal statement its frozen baselines were captured with.
    rs.device   = m_gpu->GraphFlavor() ? nullptr : m_gpu->Device().Nvrhi();
    rs.backend  = m_gpu->GraphFlavor() ? m_config.backend : m_gpu->Device().Backend();
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
    //
    // NONE OF IT ON THE GRAPH FLAVOR (NRI Phase 3, Task 6): BootPresenter
    // draws through the NVRHI swapchain + ImGui renderer, and neither exists
    // there. The window is revealed by MainLoop instead, as soon as the graph
    // vehicle that owns its only swapchain has been created -- see the reveal
    // comment there for why that is the same "never show a window nothing can
    // draw into" rule expressed against a different device.
    if (!m_gpu->GraphFlavor())
    {
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
    }
    // Disarm BEFORE Close() -- see EditorApp::StageSplashReady's matching
    // comment for why this specific ordering is required.
    m_splashPresenter.Disarm();
    if (m_splash) m_splash->Close();

    // Diagnostics dump dir (GPU crash diagnostics arc, Task 8; F-6 in the
    // seam-facts survey). The editor's equivalent call sits immediately
    // after RetargetLayoutIni() at each of ITS call sites (EditorApp.cpp /
    // EditorAppProject.cpp) -- ArcaneRuntime has no layout ini to retarget
    // (no ImGui settings file of its own) and no runtime project-switch
    // capability (one --project, decided once at boot, per RuntimeApp.hpp's
    // teardown-contract comment), so there is exactly ONE call site: here,
    // in "finalize", the same terminal core-stage id the editor's own boot
    // path retargets from (ProjectBoot.cpp's CoreStages -- "finalize" is the
    // last stage both hosts run). project_open (a "finalize" DAG ancestor
    // via plugin_load) has already settled CurrentProject() by this point,
    // one way or the other -- see ProjectBoot.cpp's RuntimeStages Fatal-ABI-
    // refusal override for what "settled" means on this host: an opened
    // project, or none, never a partial one.
    Arcane::Diagnostics::RetargetDumpDir(
        m_runtime && m_runtime->CurrentProject()
            ? m_runtime->CurrentProject()->Root() / "Saved" / "Diagnostics"
            : std::filesystem::path{});
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
    // Hoisted out of the loop body (NRI Phase 3, Task 4): BuildHud acquires
    // it, RenderNvrhi and CaptureTail both read it -- three of the six
    // extracted RuntimeFrame functions need it to survive one iteration.
    // BuildHud resets it to nullptr at the same source spot the old inline
    // `nvrhi::ITexture* backbuffer = nullptr;` declaration sat, every frame.
    nvrhi::ITexture* backbuffer = nullptr;
    bool running = true;

    // --nri-graph (NRI Phase 2, Task 7): THE RENDER HALF SWAPS, HERE.
    //
    // Not a pre-boot early-return: everything above this line already ran --
    // project, plugin, boot scene, the scene resolver and its compile service
    // -- and everything below it still runs, except that the
    // NVRHI record/submit/present half is replaced by one graph frame. That is
    // what makes a stage-golden comparison against the NVRHI baselines mean
    // anything: both paths render THE SAME booted scene.
    //
    // SINCE NRI PHASE 3 TASK 6 THIS IS ALSO THE PROCESS'S ONLY DEVICE: the
    // boot above ran GpuContext::CreateForGraph, which built no NVRHI device
    // at all, so the vehicle below wraps the first and only one -- over the
    // HOST's window, which it borrows and must not outlive (m_graphContext is
    // declared after m_gpu, so it is destroyed first).
    //
    // Built before the golden warm-up on purpose -- the warm-up can take
    // seconds on a cold toolchain, and a vehicle that cannot even create a
    // device should say so first.
    //
    // The latch baseline is taken HERE, not at process start: boot-time errors
    // belong to the boot, and everything from this point until the vehicle is
    // destroyed belongs to the graph. ShutdownGraphPath() reads it back after
    // the last NRI object is gone -- a teardown-only validation error must
    // still fail the run. This whole vehicle boot is unconditional as of
    // Phase 5a (Task 2b): the NRI frame graph is the only render path, in
    // every configuration including Dist, so there is no longer a plain-NVRHI
    // arm for it to be skipped in favour of.
    m_graphErrorBaseline = Arcane::RenderErrorCount();
    Arcane::Diagnostics::SetPhase("nri graph vehicle boot");

    // THE REVEAL, which StageFinalize could not do on this path (its
    // BootPresenter draws through an NVRHI swapchain + ImGui renderer, and
    // neither exists here). It happens BEFORE the create below, which is
    // the ORDER the Phase-2 vehicle proved at three desk checkpoints: its
    // own window was created VISIBLE and its swapchain built over an
    // already-shown window. Keeping that order rather than showing
    // afterwards is deliberate -- a surface created against a window the
    // compositor has never mapped is exactly the kind of backend-specific
    // corner a desk-only machine cannot pre-clear. The window is undrawn
    // for the handful of milliseconds until the first graph frame, the
    // same gap the vehicle always had. Show() also RAISES, which is the
    // launch reveal this host owes exactly once.
    m_gpu->Win().Show();

    m_graphContext = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
    if (!m_graphContext)
    {
        ARC_ERROR("the graph render half could not be created");
        m_graphExit = 1;
        ShutdownGraphPath();
        return;
    }
    // Guid -> asset file, so the graph path can make a REGISTERED sprite
    // material's declared TEXTURES resident on its own device (Task 9).
    // Deliberately the same lambda SceneRenderResolver builds
    // (SceneRenderResolver.cpp's constructor): re-reads CurrentProject()
    // per call, so it survives a project switch, and resolves through the
    // one registry both render paths already agree on.
    m_graphContext->SetAssetResolver(
        [rt = &*m_runtime](const Arcane::Guid& id)
            -> std::optional<std::filesystem::path>
        {
            const Arcane::Project* project = rt ? rt->CurrentProject() : nullptr;
            return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(id))
                           : std::nullopt;
        });
    // ...and the SAME seam extended to PIXELS (NRI Phase 3, Task 2): the
    // graph device cannot sample a texture on the engine's NVRHI device,
    // so its NriTextureCache uploads its own from the engine's RETAINED
    // decode (Assets::PixelsFor, Task 1) -- one decode, two devices. This
    // is what makes a textured sprite render on the graph path at all, and
    // it works with no NVRHI device present at all, which is what Task 6
    // needs.
    m_graphContext->SetPixelSupply(
        [rt = &*m_runtime](const Arcane::Guid& id) -> const Arcane::PixelData*
        {
            return rt ? rt->AssetsFacade().PixelsFor(id) : nullptr;
        });

    // Golden warm-up (NRI Phase 2): settle the scene's material compiles BEFORE
    // the frame counter starts, so what the captured/compared frame contains is
    // a function of the scene, not of how fast this machine is. See
    // DrainSceneCompiles for the failure this closes. Golden-mode-gated: an
    // ordinary run keeps binding materials opportunistically a few frames in,
    // exactly as before.
    //
    // UNCHANGED on the --nri-graph path, deliberately and by construction: the
    // warm-up drives the RESOLVER (compile -> drain -> bind), which is engine
    // state the graph path boots exactly like the NVRHI path, and the census
    // it refuses on counts resolver-level binding rather than anything drawn.
    // So a graph golden run is held to the same "the scene's materials all
    // bound before frame 1" bar even though Task 7's clear-only frame draws
    // none of them -- which is the point: the bar must already hold when Task
    // 8's node starts drawing them.
    if (m_config.GoldenMode() && m_resolver)
    {
        Arcane::Diagnostics::SetPhase("golden compile warm-up");
        // THE FRAME'S EXTENT, from whichever surface this run actually has:
        // the graph swapchain on the graph path (there is no NVRHI canvas
        // there), the NVRHI canvas otherwise. Both track the ONE window.
        const float warmupW = m_graphContext ? (float)m_graphContext->Swap().Width()
                                             : (float)m_gpu->Cnv().Width();
        const float warmupH = m_graphContext ? (float)m_graphContext->Swap().Height()
                                             : (float)m_gpu->Cnv().Height();
        if (!Arcane::DrainSceneCompiles(*m_resolver, m_shaderCompiler, warmupW, warmupH))
        {
            ARC_ERROR("golden: shader compiles did not settle within {:.0f}s -- refusing to "
                      "capture or compare a frame whose content is not bound yet",
                      Arcane::kGoldenWarmupTimeoutSeconds);
            m_goldenExit = 3;
            ShutdownGraphPath();
            return;
        }

        // Quiescent is not the same as complete: a compile that FAILED also
        // leaves the service idle. Check what the scene actually got, because
        // the whole point of the warm-up is that the artifact contains the
        // scene -- and a golden is the one place where "it rendered something"
        // is not good enough.
        const Arcane::SceneRenderResolver::MaterialCensus census = m_resolver->Materials();
        // WHAT "the post chain bound" MEANS depends on which recorder this run
        // has, and getting that wrong would refuse every graph golden (NRI
        // Phase 3, Task 6). MaterialCensus::postBound asks whether a
        // FullscreenMaterialChain -- an NVRHI object -- is Ready(), which is
        // structurally false with no device. The graph recorder does not
        // consume that chain at all: it builds its own pipelines from the
        // PostChainDesc BYTES the same cache publishes device-lessly (Task 2's
        // severance), and PostDesc() is exactly the pointer RenderGraph hands
        // the post nodes every frame. So ask the question each recorder
        // actually answers. Sprite materials need no such split -- their
        // census reads the batcher's registration table, which the bytes-only
        // path fills either way.
        const bool postBound = m_graphContext ? (m_resolver->PostDesc() != nullptr)
                                              : census.postBound;
        if (census.spriteBound != census.spriteReferenced ||
            (census.postReferenced && !postBound))
        {
            ARC_ERROR("golden: the scene's materials did not all bind ({}/{} sprite material(s), "
                      "post chain {}) -- refusing rather than freezing an incomplete frame as "
                      "the baseline; the compile failures are logged above",
                      census.spriteBound, census.spriteReferenced,
                      census.postReferenced ? (postBound ? "bound" : "UNBOUND") : "none");
            m_goldenExit = 3;
            ShutdownGraphPath();
            return;
        }
        ARC_INFO("golden: scene materials settled before frame 1 -- {} sprite material(s), "
                 "post chain {}", census.spriteBound,
                 census.postReferenced ? "bound" : "none");
    }

    // Boot is over; anything the watchdog reports from here on belongs to the
    // frame loop, not to a stale boot stage.
    Arcane::Diagnostics::SetPhase("runtime frame loop");

    // Bundles the pointers/locals MainLoop owns so the six RuntimeFrame
    // functions below (its old body, extracted verbatim -- see
    // RuntimeFrame.hpp/.cpp) can read and write them without being RuntimeApp
    // members. Constructed once: none of these bindings change identity
    // frame-to-frame (m_graphContext/m_resolver are set once at boot; the
    // rest are references, so mutations through `io` land on the real
    // members/locals directly).
    Arcane::RuntimeFrame::FrameIo io
    {
        .gpu             = m_gpu.get(),
        .graph           = m_graphContext.get(),
        .resolver        = m_resolver ? &*m_resolver : nullptr,
        .runtime         = &*m_runtime,
        .plugin          = &*m_plugin,
        .app             = *this,
        .config          = m_config,
        .perf            = m_perf,
        .frameCount      = m_frameCount,
        .goldenExit      = m_goldenExit,
        .goldenCaptured  = m_goldenCaptured,
        .graphExit       = m_graphExit,
        .frameGlobals    = m_frameGlobals,
        .hostClock       = m_hostClock,
        .lastFrameDt     = m_lastFrameDt,
        .pickDrawables   = m_pickDrawables,
        .pickSelectedIds = m_pickSelectedIds,
#if !defined(ARCANE_DIST)
        .gpuFault        = m_gpuFault,
        .gpuFaultFired   = m_gpuFaultFired,
#endif
        .simPrev         = simPrev,
        .lastFrameTime   = lastFrameTime,
        .lastShaderPoll  = lastShaderPoll,
        .backbuffer      = backbuffer,
    };

    while (running)
    {
        // Heartbeat, device-lost check, this frame's window-event pump and
        // resize handling -- true means the window's close was requested or
        // the device was lost; io.skipFrame (minimized window) asks for the
        // ordinary `continue` a bare bool return cannot express by itself.
        if (Arcane::RuntimeFrame::PumpAndResize(io)) break;
        if (io.skipFrame) continue;

        // Per-frame timing + input sampling + the fixed-step sim advance.
        // io.quit covers the Pressed("quit") input-action exit, the one quit
        // path PumpAndResize's own return does not cover.
        Arcane::RuntimeFrame::AdvanceSim(io);
        if (io.quit) break;

        // ImGui BeginFrame + the host HUD + DrawUIAll...
        Arcane::RuntimeFrame::BuildHud(io);
        // ...then (NVRHI path) backbuffer acquisition + the shader hot-reload
        // poll, and the scene resolver's Refresh -- the rest of the shared
        // prep both render arms below need. Split out of BuildHud at NRI
        // Phase 3 Task 6 (its name never covered the acquisition); the
        // statement order across the two calls is exactly what it was.
        Arcane::RuntimeFrame::PrepareFrame(io);
        if (io.skipFrame) continue;

        // =============================================================
        // THE RENDER HALF. Exactly one of these two arms runs per frame --
        // NVRHI (default, and the regression floor this whole phase is
        // measured against) or the --nri-graph vehicle. Both bodies moved to
        // RuntimeFrame.cpp verbatim at NRI Phase 3 Task 4 (RenderNvrhi /
        // RenderGraph); this if/else is the one piece of the split that has
        // to stay here, since only MainLoop's own loop can break/continue.
        // =============================================================
        if (!m_graphContext)
        {
            Arcane::RuntimeFrame::RenderNvrhi(io);
        }
        else
        {
            const Arcane::NriGraphContext::FrameOutcome outcome = Arcane::RuntimeFrame::RenderGraph(io);
            if (outcome == Arcane::NriGraphContext::FrameOutcome::Failed)  break;
            if (outcome == Arcane::NriGraphContext::FrameOutcome::Skipped) continue;
        }

        // The shared tail: the plugin's hot-reload poll, the perf tick, the
        // frame counter, and the capture/screenshot/golden bookkeeping on the
        // final frame. `stop` mirrors the original `if (lastFrame) running =
        // false;` -- the while condition above ends the loop, not a break.
        if (Arcane::RuntimeFrame::CaptureTail(io))
            running = false;
    }

    // A WHOLE-RUN property, checked once here rather than per frame
    // (whole-branch review, I2) -- the mirror of EditorApp::MainLoop's own
    // post-loop check, which 38b94b76 added to the EDITOR while leaving the
    // host that IS the frozen floor without it. CaptureTail's capture block
    // fires only on `lastFrame`, so every early break above that is not
    // device-loss (exit 1) or a graph failure (exit 1) -- a window close, the
    // "quit" input action, a Skipped-frame loop that never reached --frames N
    // -- would otherwise reach here with m_goldenExit still 0 and Run() would
    // report a clean PASS having compared nothing at all.
    //
    // Skipped when GoldenMode() is off (the member stays false and unread on
    // every ordinary run) and after a warm-up refusal (both of those paths
    // return before the loop, with m_goldenExit already 3).
    if (m_config.GoldenMode() && !m_goldenCaptured)
    {
        ARC_ERROR("golden: the run finished without ever capturing a frame to compare or "
                  "write -- it ended before reaching frame {} (a window close, the quit "
                  "action, or a frame loop that never presented)", m_config.maxFrames);
        m_goldenExit = 3;
    }

    // Destroys the vehicle and folds a grown RenderErrorCount into the exit
    // code -- read AFTER the last NRI object is gone, so a teardown-only
    // validation error still fails the run.
    ShutdownGraphPath();
}

void RuntimeApp::PushSceneCamera(float viewportWidth, float viewportHeight)
{
    // Scene camera: the ACTIVE Camera entity owns the view. Pushed HERE, after
    // the plugin's update ran, so a scene that ships a camera beats a plugin
    // that also pushes one -- the scene is the authored artifact. No camera
    // leaves the stored camera untouched (a plugin that drives the camera
    // itself via Runtime::SetCamera therefore still works) and says so
    // once, rather than substituting an identity view that would render an
    // older scene as an unexplained black window.
    int camCount = 0;
    const auto view = Arcane::ActiveSceneCamera(m_runtime->Registry(),
                                                viewportWidth, viewportHeight,
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

void RuntimeApp::ShutdownGraphPath()
{
    if (!m_graphContext)
        return;

#if !defined(ARCANE_DIST)
    // --pick-probe's ANSWER, read while the vehicle is still alive (the reset
    // below takes the readback buffer with it) and reported as an exit code so
    // a desk battery item is one scriptable line.
    //
    // 0/1 on hit/miss is the flag's own contract, and it is deliberately
    // SUBORDINATE to the run's: a probe that never got to report because the
    // run died already has m_graphExit == 1, and the errors-grew case sets 2
    // below. So this only ever turns a CLEAN run into a miss.
    if (m_config.pickProbe)
    {
        const std::optional<std::uint32_t> id = m_graphContext->ProbeId();
        if (!id.has_value())
        {
            ARC_ERROR("[nri-graph] --pick-probe ({}, {}): NO READBACK LANDED -- the run was too "
                      "short (the copy lands a couple of frames after the pass that wrote it) or "
                      "the probe pixel was outside the surface",
                      m_config.pickProbeX, m_config.pickProbeY);
            if (m_graphExit == 0)
                m_graphExit = 1;
        }
        else if (*id == 0u)
        {
            ARC_INFO("[nri-graph] --pick-probe ({}, {}): id 0 -- BACKGROUND (miss); {} pickable(s) "
                     "were in the scene", m_config.pickProbeX, m_config.pickProbeY,
                     m_pickDrawables.size());
            if (m_graphExit == 0)
                m_graphExit = 1;
        }
        else
        {
            const Astra::Entity hit = Arcane::PickEntityForId(m_pickDrawables, *id);
            ARC_INFO("[nri-graph] --pick-probe ({}, {}): id {} -- HIT (entity {}) of {} pickable(s)",
                     m_config.pickProbeX, m_config.pickProbeY, *id,
                     hit.IsValid() ? std::to_string(hit.GetID()) : std::string("<unmapped>"),
                     m_pickDrawables.size());
        }
    }
#endif

    // This reset is what destroys every NRI object (graph, cache, ring,
    // swapchain, NRI device, native device), and teardown ordering is exactly
    // the class of mistake a validation layer exists to catch -- so the latch
    // is sampled strictly after it, never from inside a still-live vehicle.
    //
    // The WINDOW is not in that list any more (NRI Phase 3, Task 6): the
    // vehicle borrows the host's, and this reset is precisely what guarantees
    // the swapchain naming its HWND/surface dies while it is still alive --
    // MainLoop calls this on every exit path, and m_gpu outlives it regardless
    // (member order).
    m_graphContext.reset();

    const std::uint64_t errorsNow = Arcane::RenderErrorCount();
    ARC_INFO("[nri-graph] RenderErrorCount {} -> {}", m_graphErrorBaseline, errorsNow);
    if (errorsNow > m_graphErrorBaseline)
    {
        ARC_ERROR("[nri-graph] FAILED: {} validation/render error(s) fired during the run "
                  "(teardown included)", errorsNow - m_graphErrorBaseline);
        // Precedence 1 > 2 > 3: a run failure says WHERE the run died and
        // outranks the errors it produced on the way out; a validation error
        // explains a bad capture rather than the reverse, so it outranks the
        // golden exit.
        if (m_graphExit == 0)
            m_graphExit = 2;
    }
}

void RuntimeApp::Shutdown()
{
    // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
    // the guard covers a future partial-init/destructor path. The GRAPH flavor
    // has no NVRHI device to idle, and needs none: ~NriGraphContext already
    // idled the one device this process had (and drained its graveyard) before
    // MainLoop returned.
    if (m_gpu && !m_gpu->GraphFlavor()) m_gpu->Device().Nvrhi()->waitForIdle();
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
    // A device-loss exit is an abnormal end even though it was orderly: the
    // report exists, but the session did not do what it was asked to.
    if (Arcane::GpuDeviceLostObserved()) return 1;
    // --nri-graph's own codes, ahead of the golden one: 1 = the graph run
    // FAILED (it says WHERE the run died), 2 = RenderErrorCount GREW (a
    // validation error fired, which explains a bad capture rather than the
    // reverse). Precedence 1 > 2 > 3, the smoke's -- see ShutdownGraphPath.
    // Always 0 when --nri-graph was not given.
    if (m_graphExit != 0) return m_graphExit;
    return m_goldenExit;   // 0 ordinarily; 3 = golden capture/compare failure
}
