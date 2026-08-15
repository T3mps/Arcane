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
        cli.Option("golden-capture", "", "write the last rendered frame to <dir>/<name>.png (pairs with --frames)");
        cli.Option("golden-compare", "", "compare the last rendered frame against <dir>/<name>.png; exit 3 on mismatch");
        cli.Option("golden-name",    "", "golden artifact stem (default: main-<backend>)");
        // Registered NEXT TO the other golden flags and, like them, NOT
        // Dist-guarded: --golden-capture/--golden-compare/--golden-name all
        // exist in every configuration, and a vocabulary where a Dist build
        // accepts --golden-capture but rejects --golden-stage would be a worse
        // trap than the shipped flag itself. (--crash-gpu is Dist-guarded
        // because it is DEV scaffolding with no golden peer; --nri-graph is
        // Dist-guarded too, but unlike --crash-gpu it DOES reach the golden
        // harness -- see the refusal rules below.)
        cli.Option("golden-stage", "full",
                   "which slice of the frame the golden covers: full|batch|post "
                   "(batch = no post chain and no ImGui, post = no ImGui)")
            .Choices({ "full", "batch", "post" });
        cli.Flag  ("print-engine-info",       "print engine identity JSON to stdout and exit");
#if !defined(ARCANE_DIST)
        cli.Option("crash-gpu", "0", "DEV: deliberately fault the GPU on frame N (0 = off) -- "
                                     "the crash-diagnostics desk trigger").Type(CliType::Uint);
        cli.Flag  ("nri-graph",      "DEV: render through the NRI frame graph instead of NVRHI "
                                     "(boots the real engine; honours --frames/--screenshot/"
                                     "--golden-*; BOTH hosts -- in the editor it currently "
                                     "renders the viewport scene only, see HostConfig.hpp)");
        // Registered beside --nri-graph rather than beside the golden flags,
        // because unlike those it is meaningless without it: the pick and
        // outline nodes exist only on the graph path. Same non-Dist guard for
        // the same reason.
        cli.Option("pick-probe", "",  "DEV (--nri-graph): add the pick + JFA outline nodes, scripted "
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
        cfg.goldenCapturePath = r.Get("golden-capture");
        cfg.goldenComparePath = r.Get("golden-compare");
        cfg.goldenName        = r.Get("golden-name");
        // Cli::Choices already refused anything outside the set at parse time
        // (stderr + exit 2), so this mapping only ever sees the three legal
        // spellings -- "full" is both the default and the else branch.
        {
            const std::string stage = r.Get("golden-stage");
            cfg.goldenStage = stage == "batch" ? GoldenStage::Batch
                            : stage == "post"  ? GoldenStage::Post
                                               : GoldenStage::Full;
        }
        cfg.printEngineInfo = r.Flag("print-engine-info");
#if !defined(ARCANE_DIST)
        cfg.crashGpuFrame = r.GetAs<std::uint64_t>("crash-gpu");
        cfg.nriGraph      = r.Flag("nri-graph");
#endif

        // Golden capture/compare only ever runs at the last frame (RuntimeApp
        // gates it on `lastFrame`, which requires maxFrames != 0). Without
        // --frames, --golden-capture/--golden-compare would silently exit 0
        // having captured/compared nothing -- refuse at parse time instead,
        // matching Cli's own error idiom (stderr + exit 2).
        if (cfg.GoldenMode() && cfg.maxFrames == 0)
        {
            std::fprintf(stderr, "error: golden capture/compare requires --frames N\n");
            return { std::nullopt, 2 };
        }

        // Same silent-no-op reasoning as the --frames rule above: the stage
        // semantics are read ONLY inside RuntimeApp's `GoldenMode()` guard, so
        // `--golden-stage batch` on an ordinary run would draw the full frame
        // anyway and exit 0, looking exactly like a batch-only run that
        // happened to include the post chain and the HUD. Refuse it here
        // instead. (`--golden-stage full` is the default and stays legal
        // everywhere -- it asks for nothing that does not already happen.)
        if (cfg.goldenStage != GoldenStage::Full && !cfg.GoldenMode())
        {
            std::fprintf(stderr, "error: --golden-stage batch|post only applies to a golden run; "
                                 "pass --golden-capture or --golden-compare\n");
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

                // Silent-no-op refusals, the same class as --golden-stage
                // outside golden mode: the pick/outline nodes live only in
                // NriGraphContext's frame, and the readback lands with
                // frames-in-flight latency so an open-ended run never reports.
                if (!cfg.nriGraph)
                {
                    std::fprintf(stderr, "error: --pick-probe only applies to the NRI graph path; "
                                         "pass --nri-graph\n");
                    return { std::nullopt, 2 };
                }
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
