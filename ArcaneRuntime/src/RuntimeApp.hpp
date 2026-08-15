#pragma once
// RuntimeApp: the standalone host application object. Constructed in main from
// a HostConfig; Run() drives Init -> the frame loop -> Shutdown and returns the
// process exit code. Member declaration order m_gpu -> m_runtime -> m_plugin is
// the TEARDOWN CONTRACT (destruct reverse: plugin Unload while the DLL is still
// mapped -> runtime -> the render stack inside GpuContext -> window last). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/GpuContext.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/SceneRenderResolver.hpp>
#include <Arcane/Host/BootPresenter.hpp>
#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/GpuFaultInjector.hpp>   // dev-only --crash-gpu N
#include <Arcane/Render/Nri/NriGraphContext.hpp>   // dev-only --nri-graph vehicle
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
namespace Astra { class TypeContext; }
class RuntimeApp
{
public:
    // `splash` is a NON-OWNING pointer to main()'s stack-local
    // Arcane::BootSplashWindow (Task 8, async-boot arc) -- same ordering
    // contract as EditorApp's ctor: main constructs and ultimately closes it;
    // RuntimeApp only reads it during Run()'s boot sequence. Null tolerated.
    explicit RuntimeApp(Arcane::HostConfig cfg, Arcane::BootSplashWindow* splash = nullptr);
    int Run();   // BootSequence -> MainLoop() -> Shutdown(); process exit code

    // Pushes the scene's ACTIVE Camera entity into the plugin's stored camera,
    // with the once-only diagnostics for "no usable camera" / "several".
    // Extracted verbatim from MainLoop's NVRHI block by NRI Phase 2 Task 8 so
    // the graph path -- which now also drives the plugin's render submission --
    // gets the same view instead of a second copy of this reasoning.
    //
    // Public since NRI Phase 3 Task 4: MainLoop's frame body moved to free
    // functions in namespace Arcane::RuntimeFrame (RuntimeFrame.hpp/.cpp), and
    // both render arms (RenderNvrhi/RenderGraph) call this through
    // FrameIo::app -- a free function cannot reach a private RuntimeApp
    // member. See RuntimeFrame.hpp's header comment.
    void PushSceneCamera(float viewportWidth, float viewportHeight);
private:
    // ---- Boot (RuntimeApp.cpp) -------------------------------------------
    // Run() builds Arcane::HostBoot::RuntimeStages(ctx) -- the SAME shared
    // function BootStageParityTest exercises and EditorApp calls for its own
    // list -- then overwrites the ids below with these closures, because
    // their real work touches RuntimeApp's own private members. See
    // ProjectBoot.cpp's CoreStages header comment for the full rationale;
    // type_context_install/project_open/input_config are NOT in this list --
    // their RuntimeStages/CoreStages body is genuinely shared and used as-is
    // (project_open's Fatal-ABI-refusal override lives IN RuntimeStages
    // itself, not here -- see ProjectBoot.cpp). StageFinalize joined this
    // list in Task 8c (2026-07-30 correction): it now performs the window
    // reveal (see its own comment), which RuntimeStages' "finalize" id used
    // to have patched to an explicit no-op.
    bool StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx);
    bool StageGpuCore(Arcane::HostBoot::BootContext& ctx);
    bool StageRenderBridge(Arcane::HostBoot::BootContext& ctx);
    bool StageSpriteTables(Arcane::HostBoot::BootContext& ctx);
    bool StagePluginLoad(Arcane::HostBoot::BootContext& ctx);
    bool StageFinalize(Arcane::HostBoot::BootContext& ctx);

    void MainLoop();
    void Shutdown();

    // --nri-graph (NRI Phase 2, Task 7): destroy the vehicle and fold a grown
    // RenderErrorCount into m_graphExit. Idempotent, and a no-op when the flag
    // was not given -- MainLoop calls it on EVERY exit path (including the
    // golden warm-up's early returns), because the latch must be read after
    // the last NRI object is gone.
    void ShutdownGraphPath();

    Arcane::HostConfig                  m_config;
    std::unique_ptr<Arcane::GpuContext> m_gpu;          // destructs LAST among engine state

    // --nri-graph's whole render half: its own window, native device, NRI
    // wrap, swapchain, upload ring, pipeline cache and RenderGraph. Null on
    // every ordinary run -- the NVRHI path is the default and stays so until
    // Phase 3 flips the hosts.
    //
    // NOT #if-guarded even though the FLAG is non-Dist: the type is compiled
    // into the engine DLL in every configuration (it is ordinary Render/Nri
    // source), and a preprocessor-guarded MEMBER would force every use site in
    // MainLoop's frame body to grow a guard of its own -- which is exactly how
    // a Dist-only compile break gets introduced. Only the CREATION is guarded,
    // so a Dist build carries a null pointer and one predictable branch.
    //
    // Declared AFTER m_gpu so it destructs BEFORE it: this object owns an SDL
    // window, and the host's window (inside m_gpu) is the one that owns the
    // SDL video subsystem's lifetime.
    std::unique_ptr<Arcane::NriGraphContext> m_graphContext;

    // Pre-device splash (Task 8): non-owning, see the ctor's doc comment.
    // Task 8c: this is now BootSequence::Run's presenter for the WHOLE boot,
    // not merely a pre-device stand-in. The old LazyBootPresenter nested
    // class that used to live here is gone -- see EditorApp.hpp's matching
    // comment for why it is no longer needed by either host.
    Arcane::BootSplashWindow*             m_splash = nullptr;
    // A class member, not a Run()-local (2026-07-30 review round 2, finding
    // 2): StageFinalize needs to call Disarm() on THIS exact instance right
    // before it closes the splash itself -- see EditorApp.hpp's matching
    // comment for the full reasoning (identical here). Constructed from
    // m_splash in the ctor's init list (declared right after it, so
    // declaration order matches).
    Arcane::BootSplashPresenter           m_splashPresenter;
    // Cannot be constructed before StageGpuCore builds m_gpu. Emplaced
    // lazily inside StageFinalize, the one place it is used now -- see that
    // method's body.
    std::optional<Arcane::BootPresenter>  m_presenter;

    Astra::TypeContext*                 m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
    // engaged by the boot sequence before MainLoop()/Shutdown() touch them (bare -> deref is safe).
    std::optional<Arcane::Runtime>      m_runtime;      // destructs before m_gpu
    std::optional<Arcane::PluginHost>   m_plugin;       // destructs before m_runtime
    Arcane::FramePerf                   m_perf;
    std::uint64_t                       m_frameCount = 0;
    // NRI Phase 0 golden harness: 0 ordinarily; set to 3 by the last-frame
    // golden capture/compare block (RuntimeApp.cpp MainLoop) on any capture
    // write failure or compare mismatch. Read by Run()'s tail as the process
    // exit code (device-loss stays exit code 1, checked first).
    int                                  m_goldenExit = 0;

    // --nri-graph's exit code, which OUTRANKS m_goldenExit (Run()'s tail):
    // 1 = the graph run failed, 2 = RenderErrorCount grew during it. 0 on
    // every run that did not pass the flag. The latch baseline it is measured
    // against is taken at the top of MainLoop -- boot-time errors belong to
    // the boot, not to the vehicle.
    int                                  m_graphExit  = 0;
    std::uint64_t                        m_graphErrorBaseline = 0;

    // --pick-probe (NRI Phase 2, Task 11). THIS is the id<->entity table: the
    // k-th drawable's hit-proxy id is k+1 (CollectPickables' ordering
    // contract), so the vector the graph's pick node rasterised is the same one
    // PickEntityForId inverts when the readback lands. Rebuilt every frame of a
    // probe run and NOT cleared afterwards -- ShutdownGraphPath reads it to name
    // the entity behind the id.
    //
    // NOT #if-guarded, for the same reason m_graphContext is not: a
    // preprocessor-guarded member forces a guard at every use site, which is
    // exactly how a Dist-only compile break gets introduced. Only the code that
    // FILLS them is guarded, so a Dist build carries two empty vectors.
    std::vector<Arcane::PickDrawable>    m_pickDrawables;
    std::vector<std::uint32_t>           m_pickSelectedIds;

#if !defined(ARCANE_DIST)
    // --crash-gpu N (GPU crash diagnostics arc, Task 11): the desk battery's
    // item-2 trigger -- the same deliberate fault the editor's Build ->
    // Diagnostics menu item fires, on the host that has no menu. Built lazily at
    // the firing frame, exactly like the editor's, so an ordinary run pays
    // nothing. Declared AFTER m_gpu so its NVRHI handles release before the
    // device (this file's teardown contract).
    std::unique_ptr<Arcane::GpuFaultInjector> m_gpuFault;
    bool                                      m_gpuFaultFired = false;
#endif

    // Scene asset resolution (sprite-resolution lift, 2026-07-29): the sprite,
    // sprite-material and post-chain caches, plus the compile drain, in ONE
    // engine-side service the Arcane Editor drives identically. Before this the
    // host published NEITHER resolution table, so a textured sprite was
    // structurally undrawable here whatever the scene contained -- see
    // docs/superpowers/plans/2026-07-29-sprite-resolution-lift.md. It also
    // retires the INTERIM post-chain wiring this block used to hold (the
    // 2026-07-23 fold-into-Arcane directive named this host as the convergence
    // vehicle, and this is that convergence).
    //
    // DECLARED LAST = DESTRUCTS FIRST, which the resolver's header requires: the
    // registry inside m_runtime holds non-owning pointers to its tables, and it
    // holds nvrhi keep-alive texture handles that must release before m_gpu's
    // device. The compile service + template source roots feed it and are
    // declared before it for the same reason.
    Arcane::ShaderCompiler       m_shaderCompiler;
    Arcane::ShaderSourceProvider m_shaderSources;
    std::optional<Arcane::SceneRenderResolver> m_resolver;
    Arcane::GlobalParams                       m_frameGlobals{};
    double m_hostClock   = 0.0;   // seconds since the loop started (compile debounce clock)
    double m_lastFrameDt = 0.0;   // last frame's wall-clock delta, for the material globals

    // Scene-camera warnings, latched so a per-frame condition is reported ONCE and
    // does not bury the log it would be found in (the same warn-once shape the
    // resolver's PostProcess sweep uses). Never reset: a host run is one scene's
    // worth of complaint, and the log is truncated per launch anyway.
    bool m_warnedNoSceneCamera    = false;
    bool m_warnedMultiSceneCamera = false;
};
