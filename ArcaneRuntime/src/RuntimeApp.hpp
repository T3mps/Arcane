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
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
namespace Astra { class TypeContext; }
class RuntimeApp
{
public:
    explicit RuntimeApp(Arcane::HostConfig cfg);
    int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code
private:
    bool Init();
    void MainLoop();
    void Shutdown();

    Arcane::HostConfig                  m_config;
    std::unique_ptr<Arcane::GpuContext> m_gpu;          // destructs LAST among engine state
    Astra::TypeContext*                 m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
    // engaged by Init() before MainLoop()/Shutdown() touch them (bare -> deref is safe).
    std::optional<Arcane::Runtime>      m_runtime;      // destructs before m_gpu
    std::optional<Arcane::PluginHost>   m_plugin;       // destructs before m_runtime
    Arcane::FramePerf                   m_perf;
    std::uint64_t                       m_frameCount = 0;

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
