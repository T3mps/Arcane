#pragma once
// RuntimeApp: the standalone host application object. Constructed in main from
// a HostConfig; Run() drives Init -> the frame loop -> Shutdown and returns the
// process exit code. Member declaration order m_gpu -> m_runtime -> m_plugin is
// the TEARDOWN CONTRACT (destruct reverse: plugin Unload while the DLL is still
// mapped -> runtime -> the render stack inside GpuContext -> window last). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <memory>
#include <optional>
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

    Arcane::HostConfig                  m_config;
    std::unique_ptr<Arcane::GpuContext> m_gpu;          // destructs LAST among engine state

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
