#pragma once
// Loom: the application object. Constructed in main from a LoomConfig; Run() drives
// Init -> the frame loop -> Shutdown and returns the process exit code. Member
// declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN CONTRACT
// (destruct reverse: plugin Unload while the DLL is still mapped -> runtime ->
// the render stack inside GpuContext -> window last). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <memory>
#include <optional>
#include "LoomConfig.hpp"
#include "GpuContext.hpp"
#include "FramePerf.hpp"
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
namespace Astra { class TypeContext; }
class Loom
{
public:
    explicit Loom(LoomConfig cfg);
    int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code
private:
    bool Init();
    void MainLoop();
    void Shutdown();

    LoomConfig                        m_config;
    std::unique_ptr<GpuContext>       m_gpu;          // destructs LAST among engine state
    Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
    // engaged by Init() before MainLoop()/Shutdown() touch them (bare -> deref is safe).
    std::optional<Arcane::Runtime>    m_runtime;      // destructs before m_gpu
    std::optional<Arcane::PluginHost> m_plugin;       // destructs before m_runtime
    FramePerf                         m_perf;
    std::uint64_t                     m_frameCount = 0;
};
