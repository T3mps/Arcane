// ArcaneServer -- the Core-only dedicated-server host's entry point (Core-DLL
// split, spec docs/specs/2026-09-15-core-dll-split-design.md s6, plan 1
// Task 6). Parses argv into a ServerConfig, constructs the ServerApp
// application object, and returns its Run() exit code -- mirrors
// ArcaneRuntime/src/main.cpp's own shape, minus everything presentation-only:
// no boot splash (there is no window), no --dump-layout refusal (there is no
// ImGui layout), and no D3D12 Agility SDK export (there is no D3D12 device in
// this process at all). The link line is the construction proof this host is
// Core-only: see premake5.lua's ArcaneServer block (links ArcaneCore, NOT
// ArcaneClient).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::PluginABIVersion (the --print-engine-info probe)
#include "ServerApp.hpp"
#include "ServerConfig.hpp"

#include <Json.hpp>

#include <cstdio>

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();

    const Arcane::Server::ServerConfig::ParseOutcome parsed =
        Arcane::Server::ServerConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2

    // Same probe every host offers: identity to stdout, no window, no device,
    // no project, no ProcessContext. `EngineInfoJson` (ArcaneClient/src/Arcane/
    // Host/ProjectBoot.hpp) stays Client-side deliberately -- this Core-only
    // host does not link Client for one string function, so it prints the
    // SAME three keys itself, straight from the Core-side identity probes
    // (Arcane::PluginABIVersion/BuildInfo/ExecutablePathUtf8) EngineInfoJson
    // is built from.
    if (parsed.config->printEngineInfo)
    {
        nlohmann::json j;
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = Arcane::ExecutablePathUtf8();
        std::printf("%s\n", j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
        return 0;
    }

    // Same arming as the other hosts, same reasoning: a crash or a hang on
    // this host must leave the same evidence behind as ArcaneRuntime/
    // ArcaneEditor do.
    {
        Arcane::Diagnostics::Config diag;
        diag.appName = "ArcaneServer";
        Arcane::Diagnostics::Install(diag);
    }

    int rc = 0;
    {
        Arcane::Server::ServerApp app(*parsed.config);
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("server teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
