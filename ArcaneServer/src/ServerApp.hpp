#pragma once

// ServerApp: the dedicated-server host application object (Core-DLL split,
// spec docs/specs/2026-09-15-core-dll-split-design.md s6, plan 1 Task 6).
// Owns the process's ONE ProcessContext, the ONE authoritative Runtime
// (NetMode::DedicatedServer) and the PluginHost that watches the project's
// game module. Run() opens the project, loads the module, boots the boot
// scene, then ticks a fixed-step loop -- forever or --frames N -- and always
// returns having tried to write the `--report` census, whichever step it
// died on.
//
// CORE-ONLY, BY CONSTRUCTION: this type includes nothing from ArcaneClient
// (no HostConfig, no GpuContext, no ClientRuntime) -- see main.cpp and
// premake5.lua's ArcaneServer block for the link-line half of that proof.

#include "ServerConfig.hpp"
#include "ServerReport.hpp"

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

#include <memory>
#include <optional>

namespace Arcane::Server
{
    class ServerApp
    {
    public:
        explicit ServerApp(ServerConfig cfg) : m_cfg(std::move(cfg)) {}

        // Opens the project, loads the module, ticks the fixed-step loop, and
        // writes the `--report` census on every exit path (when --report was
        // given). Returns the process exit code (0 on a clean "frames-complete"
        // finish, non-zero on any refusal).
        int Run();

    private:
        // Finish: stamp the census's exitReason, write it to m_cfg.reportPath
        // (if any), and return `exitCode` -- the ONE tail every Run() return
        // path funnels through, so a report is attempted on every outcome,
        // not only the happy path.
        int Finish(ServerReport& rep, std::string exitReason, int exitCode);

        ServerConfig m_cfg;

        // Declaration order is the teardown contract, same shape as every
        // other host's (RuntimeApp.hpp's own comment): m_process outlives
        // m_runtime, and m_runtime outlives m_plugin -- so m_plugin's dtor
        // (Unload while the module DLL is still mapped) runs first, then
        // ~Runtime, then ~ProcessContext.
        std::unique_ptr<Arcane::ProcessContext> m_process;
        std::optional<Arcane::Runtime>          m_runtime;
        std::optional<Arcane::PluginHost>       m_plugin;
    };
}
