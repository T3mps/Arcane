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
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
namespace Astra { class TypeContext; }
namespace Arcane { class FullscreenMaterialChain; class MaterialInstance; }
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

    // Scene post chain (post arc, slice 2): the canvas -> chain -> tonemap
    // hook is live in MainLoop; these stay null until slice 3's PostProcess
    // sweep feeds them (INTERIM inline wiring per the 2026-07-23 fold-into-Arcane
    // directive -- the .arcproj runtime host is the convergence vehicle).
    // Non-owning; whatever binds them keeps them alive across the frame.
    Arcane::FullscreenMaterialChain*  m_postChain = nullptr;
    const Arcane::MaterialInstance*   m_postInstance = nullptr;
    Arcane::GlobalParams              m_postGlobals{};

    // Scene-camera warnings, latched so a per-frame condition is reported ONCE and
    // does not bury the log it would be found in (the same warn-once shape the
    // editor's PostProcess sweep uses). Never reset: a host run is one scene's
    // worth of complaint, and the log is truncated per launch anyway.
    bool m_warnedNoSceneCamera    = false;
    bool m_warnedMultiSceneCamera = false;
};
