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
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // THE RELAUNCH LINE (Diagnostics::Config::commandLine, spec S5.1), this
    // host's own copy of the rule ArcaneClient's SanitizeRelaunchLine
    // implements for the two windowed hosts.
    //
    // NOT a call into that one: this exe is Core-only BY CONSTRUCTION (see
    // this file's header comment and premake5.lua's ArcaneServer block -- it
    // links ArcaneCore, never ArcaneClient), and dragging Client in for one
    // string function would break the very property ServerReport's
    // clientDllLoadedAtBoot probe exists to police. The vocabulary is this
    // host's own too (ServerConfig, not HostConfig): --frames N and --report
    // <json> are the scripted-run half, and everything else -- --project,
    // --plugin, --fixed-dt -- describes the SESSION and is kept.
    //
    // `exePath` REPLACES argv[0], for the same reason the --print-engine-info
    // probe below prints ExecutablePathUtf8() rather than argv[0]: argv[0] is
    // whatever the launcher typed -- a bare relative name under the documented
    // cd-then-run workflow, in ANSI-codepage bytes under MSVC -- which neither
    // relaunches from the reporter's own working directory nor survives a
    // strict-UTF-8 envelope.
    std::string SanitizedRelaunchLine(const std::string& exePath, int argc, char** argv)
    {
        static constexpr std::string_view kStripWithValue[] = { "frames", "report" };
        std::string out = (exePath.find(' ') != std::string::npos || exePath.empty())
                        ? "\"" + exePath + "\""
                        : exePath;
        for (int i = 1; i < argc; ++i)   // 1: argv[0] is `exePath` above
        {
            std::string_view arg(argv[i]);
            if (arg.starts_with("--"))
            {
                std::string_view body = arg.substr(2);
                const std::size_t eq = body.find('=');           // Cli accepts --name=value too
                const bool inlineValue = eq != std::string_view::npos;
                const std::string_view name = inlineValue ? body.substr(0, eq) : body;
                bool strip = false;
                for (std::string_view s : kStripWithValue) strip = strip || (s == name);
                if (strip)
                {
                    if (!inlineValue && i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"))
                        ++i;   // its value
                    continue;
                }
            }
            if (!out.empty()) out.push_back(' ');
            if (arg.find(' ') != std::string_view::npos || arg.empty())
                out += "\"" + std::string(arg) + "\"";
            else
                out += std::string(arg);
        }
        return out;
    }
}

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
    //
    // AND BEFORE Diagnostics::Install BELOW (R25), the ONE path that exits
    // before arming -- see ArcaneEditor/src/main.cpp's copy of this note: a
    // pure query that prints one line and exits must not start the crash
    // thread and the watchdog, rotate a log file and leave an empty
    // diagnostics/ directory beside the exe.
    if (parsed.config->printEngineInfo)
    {
        nlohmann::json j;
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = Arcane::ExecutablePathUtf8();
        std::printf("%s\n", j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
        return 0;
    }

    // POST-MORTEM CAPTURE, FIRST (bar the probe above) -- crash window plan 1,
    // task 9; spec S5.1's closing paragraph. Same arming, same reasoning and
    // the same position as the other two hosts, which is the whole point: a
    // crash or a hang on this host must leave the same evidence behind.
    {
        Arcane::Diagnostics::Config diag;
        diag.appName     = "ArcaneServer";
        diag.productName = "Arcane Server";
        // ALWAYS unattended, unlike the other two: a dedicated server has no
        // desktop session to put a reporter window on, whatever its flags say.
        diag.unattended  = true;
        diag.commandLine = SanitizedRelaunchLine(Arcane::ExecutablePathUtf8(), argc, argv);
        Arcane::Diagnostics::Install(diag);
    }
    // Every host installs one (R24): with the slot empty a first Ctrl-C is
    // declined and Windows terminates outright. This is the host where it
    // matters most -- a dedicated server's ordinary stop IS a Ctrl-C or a
    // service shutdown, and its tick loop is otherwise open-ended.
    Arcane::Server::ServerApp::InstallCleanExitHook();

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
