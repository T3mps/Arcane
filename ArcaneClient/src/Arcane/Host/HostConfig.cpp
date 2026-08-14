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
        // trap than the shipped flag itself. (--crash-gpu / --nri-smoke are
        // Dist-guarded because they are DEV scaffolding with no golden peer.)
        cli.Option("golden-stage", "full",
                   "which slice of the frame the golden covers: full|batch|post "
                   "(batch = no post chain and no ImGui, post = no ImGui)")
            .Choices({ "full", "batch", "post" });
        cli.Flag  ("print-engine-info",       "print engine identity JSON to stdout and exit");
#if !defined(ARCANE_DIST)
        cli.Option("crash-gpu", "0", "DEV: deliberately fault the GPU on frame N (0 = off) -- "
                                     "the crash-diagnostics desk trigger").Type(CliType::Uint);
        cli.Flag  ("nri-smoke",      "DEV: run the NRI Phase-1 triangle smoke instead of "
                                     "booting the engine (honours --frames/--screenshot)");
        cli.Flag  ("nri-graph",      "DEV: render through the NRI frame graph instead of NVRHI "
                                     "(boots the real engine; honours --frames/--screenshot/"
                                     "--golden-*)");
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
        cfg.nriSmoke      = r.Flag("nri-smoke");
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
        // Same silent-no-op reasoning, same treatment: the golden harness lives
        // in RuntimeApp::MainLoop, which --nri-smoke never reaches (Run()
        // returns before the boot starts), so the combination would
        // capture/compare nothing and still exit 0. (--screenshot IS honoured
        // by the smoke -- only the golden flags are unreachable from it.)
        //
        // SCOPED TO THE SMOKE, deliberately (NRI Phase 2, Task 7). --nri-graph
        // DOES reach MainLoop -- it boots the whole engine and swaps only the
        // render half -- so the golden harness runs there and the flags are
        // not merely legal but the point of the mode. Never widen this
        // condition to "either NRI mode"; HostConfigTest pins both halves.
        if (cfg.nriSmoke && cfg.GoldenMode())
        {
            std::fprintf(stderr, "error: --nri-smoke does not run the golden harness; "
                                 "use --screenshot instead of --golden-capture/--golden-compare "
                                 "(--nri-graph does run it)\n");
            return { std::nullopt, 2 };
        }

        // Two whole-render-path modes at once. They are not composable and not
        // orderable: --nri-smoke returns from RuntimeApp::Run before the boot
        // starts, so a run carrying both would silently be a smoke run with
        // --nri-graph ignored. Refuse rather than pick one.
        if (cfg.nriSmoke && cfg.nriGraph)
        {
            std::fprintf(stderr, "error: --nri-smoke and --nri-graph are two different render "
                                 "paths -- pass exactly one\n");
            return { std::nullopt, 2 };
        }
#endif

        return { cfg, 0 };
    }
}
