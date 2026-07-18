#pragma once
// GrimoireApp: the editor application. Constructed in main from a LoomConfig
// (reused as the host config); Run() drives Init -> the frame loop -> Shutdown.
// Member declaration order m_gpu -> m_runtime -> m_plugin is the TEARDOWN
// CONTRACT (destruct reverse: plugin Unload while the DLL is still mapped ->
// runtime -> render stack in GpuContext -> window last). Mirrors Loom.
#include <cstdint>
#include <memory>
#include <optional>

#include <LoomConfig.hpp>
#include <GpuContext.hpp>
#include <FramePerf.hpp>
#include "ConsoleBuffer.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

namespace Astra { class TypeContext; }

namespace Grimoire
{
    class GrimoireApp
    {
    public:
        explicit GrimoireApp(LoomConfig cfg);
        int Run();   // Init() -> MainLoop() -> Shutdown(); process exit code

    private:
        bool Init();
        void MainLoop();
        void Shutdown();
        void InstallConsoleSink();   // attach a callback sink on Arcane::Log::Engine() -> m_console

        LoomConfig                        m_config;
        std::unique_ptr<GpuContext>       m_gpu;                    // destructs LAST
        Astra::TypeContext*               m_typeContext = nullptr;  // heap-leaked singleton (NOT owned)
        std::optional<Arcane::Runtime>    m_runtime;                // destructs before m_gpu
        std::optional<Arcane::PluginHost> m_plugin;                 // destructs before m_runtime
        FramePerf                         m_perf;
        std::uint64_t                     m_frameCount = 0;
        ConsoleBuffer                     m_console{512};
    };
}
