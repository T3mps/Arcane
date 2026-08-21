#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Cli/Cli.hpp>
#include <cstdio>
namespace Arcane
{
    HostConfig::ParseOutcome HostConfig::Parse(int argc, char** argv)
    {
        Cli cli{ "Arcane Runtime", "standalone runtime host" };
        cli.Option("backend", "dx12",        "graphics backend: dx12|vulkan").Choices({ "dx12", "vulkan" });
        cli.Option("frames",  "0",           "render N frames then exit").Type(CliType::Uint);
        cli.Flag  ("no-vsync",               "present without vsync");
        cli.Flag  ("perf",                   "log per-phase ms every 60 frames");
        cli.Option("plugin",  "",            "game DLL to host (empty = the project's gameModule; a runtime with nothing to host refuses boot)");
        cli.Option("project", "", "project folder or .arcproj to open (empty = data/-next-to-exe)");
        cli.Option("scene",   "", "asset Guid to boot instead of the manifest's bootScene (empty = follow the manifest)");
        cli.Option("screenshot", "", "write the last rendered frame to this PNG before exiting (pairs with --frames)");
        cli.Flag  ("print-engine-info",       "print engine identity JSON to stdout and exit");
        // NOT Dist-guarded (Phase 5a, Task 2b): the NRI frame graph is the
        // ONLY render path in every configuration now, so the flag that used
        // to select it has nothing left to select. Kept registered and
        // parsed-and-ignored rather than deleted or refused outright --
        // scripts, the Hub's saved launch args, and the desk batteries all
        // still pass it, and a hard "unknown argument" refusal would turn a
        // harmless no-op into a boot failure at the worst moment (a shipped
        // Dist build, launched from a saved arg list nobody is watching).
        cli.Flag  ("nri-graph",      "DEPRECATED, accepted and ignored: the NRI frame graph is "
                                     "the only render path as of Phase 5a. Kept so existing "
                                     "scripts and saved launch args do not fail to boot.");
#if !defined(ARCANE_DIST)
        cli.Option("crash-gpu", "0", "DEV: deliberately fault the GPU on frame N (0 = off) -- "
                                     "the crash-diagnostics desk trigger").Type(CliType::Uint);
        // Registered beside the other Dist-guarded dev flags. --nri-graph is
        // no longer this flag's prerequisite (Phase 5a: the graph path is
        // unconditional), so the help text no longer names it.
        cli.Option("pick-probe", "",  "DEV: add the pick + JFA outline nodes, scripted "
                                      "onto the scene's first pickable entity, and print the entity id "
                                      "read back at canvas pixel x,y -- exit 0 on a hit, 1 on a miss");
#endif

        const Cli::Result r = cli.Parse(argc, argv);
        if (!r.ok) return { std::nullopt, r.exitCode };

        HostConfig cfg;
        cfg.backend    = (r.Get("backend") == "vulkan") ? GraphicsBackend::Vulkan : GraphicsBackend::D3D12;
        cfg.maxFrames  = r.GetAs<std::uint64_t>("frames");
        cfg.vsync      = !r.Flag("no-vsync");
        cfg.perf       = r.Flag("perf");
        cfg.pluginPath = r.Get("plugin");
        cfg.projectPath = r.Get("project");
        cfg.sceneOverride = r.Get("scene");
        cfg.screenshotPath = r.Get("screenshot");
        cfg.printEngineInfo = r.Flag("print-engine-info");
        // "nri-graph" is intentionally never read here: it is registered
        // above (unconditionally) purely so a command line that still passes
        // it does not fail to parse. There is nothing left to store -- the
        // graph path it used to opt into is now the only one.
#if !defined(ARCANE_DIST)
        cfg.crashGpuFrame = r.GetAs<std::uint64_t>("crash-gpu");
#endif

        // --screenshot only ever fires on the last frame (both hosts gate it on
        // `lastFrame`, which requires maxFrames != 0). Without --frames it
        // would silently exit 0 having written nothing -- refuse at parse time
        // instead, matching Cli's own error idiom (stderr + exit 2).
        if (!cfg.screenshotPath.empty() && cfg.maxFrames == 0)
        {
            std::fprintf(stderr, "error: --screenshot requires --frames N\n");
            return { std::nullopt, 2 };
        }

#if !defined(ARCANE_DIST)
        // --pick-probe x,y (NRI Phase 2, Task 11). Parsed HERE rather than at
        // the use site, and refused rather than clamped, because the whole
        // value of the flag is being scriptable: a probe whose coordinate was
        // silently reinterpreted would report a confident id for a pixel
        // nobody asked about. Cli has no pair type (documented non-goal), so
        // the split is done by hand -- the refusals below are exactly the
        // parse-time treatment --backend metal gets.
        {
            const std::string probe = r.Get("pick-probe");
            if (!probe.empty())
            {
                const std::size_t comma = probe.find(',');
                bool bad = comma == std::string::npos || comma == 0 || comma + 1 >= probe.size();
                long long x = 0, y = 0;
                if (!bad)
                {
                    const std::string xs = probe.substr(0, comma);
                    const std::string ys = probe.substr(comma + 1);
                    const auto whole = [](const std::string& text, long long& out)
                    {
                        if (text.empty())
                            return false;
                        out = 0;
                        for (const char c : text)
                        {
                            if (c < '0' || c > '9')
                                return false;
                            out = out * 10 + (c - '0');
                            if (out > 0x7FFFFFFFll)
                                return false;
                        }
                        return true;
                    };
                    // Unsigned on purpose: a NEGATIVE probe pixel is not a
                    // coordinate, it is a typo -- the id target has no texels
                    // there and the run would report a miss that means nothing.
                    bad = !whole(xs, x) || !whole(ys, y);
                }
                if (bad)
                {
                    std::fprintf(stderr, "error: --pick-probe wants two non-negative integers as "
                                         "x,y (canvas pixels, y down) -- e.g. --pick-probe 640,360\n");
                    return { std::nullopt, 2 };
                }

                // Silent-no-op refusal, the same class as --golden-stage
                // outside golden mode: the readback lands with frames-in-flight
                // latency so an open-ended run never reports. Used to also
                // refuse without --nri-graph (the pick/outline nodes lived
                // only on that path) -- that guard is gone as of Phase 5a:
                // the NRI frame graph is the only render path, so the nodes
                // are always present.
                if (cfg.maxFrames == 0)
                {
                    std::fprintf(stderr, "error: --pick-probe requires --frames N (the readback "
                                         "lands a couple of frames after the pass that wrote it)\n");
                    return { std::nullopt, 2 };
                }

                cfg.pickProbe  = true;
                cfg.pickProbeX = (std::int32_t)x;
                cfg.pickProbeY = (std::int32_t)y;
            }
        }
#endif

        return { cfg, 0 };
    }
}
