#include "ServerConfig.hpp"

#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Sim/SimSettings.hpp>   // server.tickHz

#include <cmath>
#include <cstdio>

namespace Arcane::Server
{
    ServerConfig::ParseOutcome ServerConfig::Parse(int argc, char** argv)
    {
        Cli cli{ "ArcaneServer", "Core-only dedicated-server host" };
        cli.Option("project", "", "project folder or .arcproj to open (REQUIRED unless --print-engine-info)");
        cli.Option("plugin",  "", "game DLL to host (empty = the project's gameModule; a server with nothing to host refuses boot)");
        cli.Option("frames",  "0", "tick N fixed steps then exit (0 = run until terminated)").Type(CliType::Uint);
        cli.Option("fixed-dt", "", "seconds per fixed tick (empty = 1/server.tickHz)").Type(CliType::Double);
        // The --report caveat is stated in the help text, not only in a doc: the
        // report is written when the tick loop ENDS (ServerApp::Finish), so with
        // --frames 0 a server that is killed writes none. A console-control
        // handler that finishes on Ctrl+C/terminate is the follow-up.
        cli.Option("report",  "", "write the census to this JSON path (written when the "
                                  "loop ends -- with --frames 0 a killed server writes none)");
        cli.Flag  ("print-engine-info", "print engine identity JSON to stdout and exit");
        cli.Flag  ("no-admin-console", "do not read admin commands (get/set/list/explain) from stdin");

        const Cli::Result r = cli.Parse(argc, argv);
        if (!r.ok) return { std::nullopt, r.exitCode };

        ServerConfig cfg;
        cfg.projectPath     = r.Get("project");
        cfg.pluginPath      = r.Get("plugin");
        cfg.frames          = r.GetAs<std::uint64_t>("frames");
        cfg.fixedDtSupplied = r.Supplied("fixed-dt");
        cfg.fixedDtSeconds  = cfg.fixedDtSupplied ? r.GetAs<double>("fixed-dt") : 0.0;
        cfg.reportPath      = r.Get("report");
        cfg.printEngineInfo = r.Flag("print-engine-info");
        cfg.adminConsole    = !r.Flag("no-admin-console");

        // Same NaN reasoning HostConfig::Parse's --fixed-dt refusal documents:
        // `<= 0.0` alone does not reject NaN (every comparison against NaN is
        // false), so isfinite is checked explicitly. Refused, not clamped --
        // rule 3 (no silently inert/wrong flags): a caller who typed a bad
        // step size typed something they did not mean.
        if (cfg.fixedDtSupplied && (!std::isfinite(cfg.fixedDtSeconds) || cfg.fixedDtSeconds <= 0.0))
        {
            std::fprintf(stderr, "error: --fixed-dt wants a positive, finite number of seconds\n");
            return { std::nullopt, 2 };
        }

        // A server with nothing to host refuses boot, like ArcaneRuntime --
        // unless the caller only wants the engine-identity probe, which needs
        // no project at all.
        if (cfg.projectPath.empty() && !cfg.printEngineInfo)
        {
            std::fprintf(stderr, "error: --project <dir> is required (a server with nothing to host "
                                 "refuses boot; pass --print-engine-info if you only want the engine "
                                 "identity probe)\n");
            return { std::nullopt, 2 };
        }

        return { cfg, 0 };
    }

    double ServerConfig::FixedHz() const
    {
        return fixedDtSupplied ? 1.0 / fixedDtSeconds : Settings<ServerSettings>().tickHz;
    }
}
