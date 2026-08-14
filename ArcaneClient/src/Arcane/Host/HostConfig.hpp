#pragma once
// HostConfig: the typed result of parsing a runtime host's command line. Owns
// the host option vocabulary; delegates the parsing to the generic Arcane::Cli
// (Core). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <string>
#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Device.hpp>   // Arcane::GraphicsBackend
namespace Arcane
{
    // WHICH SLICE OF THE FRAME a golden capture/compare covers (NRI Phase 2).
    // The 2D cutover is verified node by node, and a whole-frame golden cannot
    // say WHICH stage regressed: a batch-node bug and a post-node bug both show
    // up as "main-dx12.png differs". Capturing the frame truncated after each
    // stage gives one golden per seam, so the first stage whose golden breaks
    // names the node.
    //
    // Semantics in the runtime frame (RuntimeApp::MainLoop):
    //   Full  -- everything (the pre-Phase-2 frame, byte for byte)
    //   Batch -- batcher + tonemap only: the post chain and the ImGui pass are
    //            both bypassed
    //   Post  -- batcher + post chain + tonemap: only the ImGui pass is bypassed
    //
    // Honoured ONLY in golden mode (see HostConfig::GoldenMode) -- an ordinary
    // run always draws the whole frame, whatever this says.
    enum class GoldenStage : std::uint8_t { Full, Batch, Post };

    struct ARCANE_API HostConfig
    {
        GraphicsBackend backend   = GraphicsBackend::D3D12;
        std::uint64_t   maxFrames = 0;             // 0 = run until quit
        bool            vsync     = true;
        bool            perf      = false;
        // Empty = "no explicit --plugin": the project's gameModule is what loads. The editor
        // tolerates having nothing to host (project-less workshop); ArcaneRuntime refuses.
        std::string     pluginPath = "";
        std::string     projectPath = "";   // .arcproj or project folder; "" = data/-next-to-exe
        // Boot this scene (asset Guid text) instead of the manifest's bootScene. Empty = follow
        // the manifest. Editor separate-window play passes the ACTIVE scene here.
        std::string     sceneOverride = "";

        // Write the LAST rendered frame to this PNG before exiting. Empty = off.
        //
        // This is the scripted GPU-verify's missing half. `--frames N` proved a host
        // could run N frames without a validation error; it could not tell anyone
        // WHAT was on screen, so "the scene renders" stayed a desk-only claim and a
        // scene that drew every sprite as a single pixel (the 2026-07-30 TypeContext
        // bug) looked identical to a healthy run in the log. Pairs with --frames:
        // `--frames 60 --screenshot out.png` is a headless visual check.
        //
        // Captures the BACKBUFFER, after tonemap and ImGui -- the actual pixels a
        // player sees, not an intermediate the eye never gets.
        std::string     screenshotPath = "";

        // Golden image testing (Task 4 of NRI Phase 0 harness).
        std::string     goldenCapturePath = "";   // --golden-capture <dir>: write <dir>/<name>.png on the last frame
        std::string     goldenComparePath = "";   // --golden-compare <dir>: compare last frame vs <dir>/<name>.png; exit 3 on mismatch
        std::string     goldenName        = "";   // --golden-name <name>: artifact stem (default "main-<backend>" at use site)

        // --golden-stage full|batch|post (NRI Phase 2). Default Full = the
        // pre-Phase-2 behaviour exactly. Only read when GoldenMode() is true;
        // HostConfig::Parse refuses a non-Full stage without golden mode rather
        // than let it be a silent no-op.
        //
        // ARTIFACT NAMING (the stem the runtime derives from this + goldenName):
        //   Full  -> "main-<backend>"          (unchanged -- Phase 0's goldens
        //                                       keep their filenames)
        //   Batch -> "main-batch-<backend>"
        //   Post  -> "main-post-<backend>"
        // An explicit --golden-name is the WHOLE stem (the cross-backend
        // compare passes the other backend's stem there), so a non-Full stage
        // appends "-<stage>" to it instead -- enough that the three stages of
        // one scripted run cannot overwrite each other's file.
        GoldenStage     goldenStage = GoldenStage::Full;

        // True if either capture or compare mode is active.
        [[nodiscard]] bool GoldenMode() const noexcept
        { return !goldenCapturePath.empty() || !goldenComparePath.empty(); }

        // Print one line of engine-identity JSON to stdout and exit, without creating
        // a window or device. The Arcane Hub probes this to learn the plugin ABI it
        // must stamp into a new .arcproj -- see HostBoot::EngineInfoJson.
        bool            printEngineInfo = false;

#if !defined(ARCANE_DIST)
        // DEV ONLY: fire the deliberate GPU fault (Render/GpuFaultInjector.hpp)
        // ONCE, on the first frame recorded after this many frames have
        // completed -- `--crash-gpu 30` faults during frame 31, which is the
        // point of the number: enough warm-up that the device, swapchain and
        // shaders are all genuinely live. 0 = off.
        //
        // The editor reaches the same injector through a menu item; ArcaneRuntime
        // has no menu, and the desk battery's item 2 -- "same, via ArcaneRuntime
        // (project-less fallback dir)" -- is unrunnable without a trigger. A flag
        // rather than a keybind because the battery is a scripted, repeatable
        // check: `ArcaneRuntime --project ReferenceProject --crash-gpu 30` is one
        // line, and it pairs with --frames the way --screenshot does.
        //
        // Honoured by BOTH hosts. The editor also has the menu item, but a flag
        // that one host parses and silently ignores is a trap, and scripting the
        // editor's battery items beats clicking them.
        //
        // KNOWN GAP, NOT REFUSED (NRI Phase 2 ledger, Task 7 minor, closed at
        // Task 13): on ArcaneRuntime, the fault injector only fires inside
        // RuntimeApp::MainLoop's NVRHI record path (`if (!m_graphContext)`);
        // the `--nri-graph` frame-graph vehicle has no fault-injector node
        // wired in, so `--crash-gpu N --nri-graph` parses, never fires, and
        // the run exits 0 having proven nothing. Left as a documented no-op
        // rather than refused at parse: wiring the injector into the graph
        // path is a graph-vehicle change, out of scope for a flag this
        // vehicle does not otherwise touch, and --nri-graph is dev-only
        // scaffolding a desk user reads this comment before scripting.
        std::uint64_t   crashGpuFrame = 0;

        // DEV ONLY (NRI Phase 2, Task 7): the frame-graph VEHICLE. Renders
        // the frame through Arcane::NriGraphContext (NRI device + swapchain +
        // upload ring + pipeline cache + RenderGraph) instead of the NVRHI
        // record/submit/present half.
        //
        // NOT the shape Phase 1's triangle smoke was: that path owned the
        // whole process and returned from RuntimeApp::Run BEFORE the boot
        // started; it was deleted at Task 13, once this vehicle covered real
        // content. --nri-graph boots the REAL engine -- project, plugin,
        // scene resolve, sim, the material compile service -- and swaps only
        // the RENDER half inside MainLoop. So everything the normal boot
        // brings (a scene, bound materials, the golden warm-up and its
        // material census) is live on this path, which is what makes
        // stage-golden comparison against the NVRHI baselines meaningful.
        //
        // Consequences, stated because they are observable:
        //   - the golden flags ARE honoured here; see Parse. Same
        //     --frames/--screenshot/--golden-*/--golden-stage vocabulary,
        //     same artifact names, same exit codes.
        //   - ArcaneRuntime-only. The editor host keeps the NVRHI path until
        //     Phase 3 flips both hosts.
        //
        // Non-Dist for the same reason --crash-gpu is: a shipped build has no
        // business carrying a dev render path.
        bool            nriGraph = false;

        // DEV ONLY (NRI Phase 2, Task 11): `--pick-probe x,y` -- the SCRIPTED
        // desk check for the graph path's pick + JFA outline nodes.
        //
        // Turning it on does three things, and all three are the point:
        //   1. the frame grows the pick node (an R32_UINT entity-id target +
        //      a 1-pixel readback) and the outline chain (seed -> N jump-flood
        //      steps -> a composite over the tonemapped backbuffer). With the
        //      flag ABSENT the frame's shape is byte-for-byte Task 10's, which
        //      is what keeps the batch/post/full stage goldens comparable;
        //   2. the SELECTION is scripted -- the first pickable drawable in the
        //      scene (hit-proxy id 1) -- because there is no editor selection
        //      on this host and Phase 3 owns the real wiring;
        //   3. the run prints the id read back at (x, y) and exits 0 on a HIT
        //      (a non-zero id) or 1 on a MISS, so a desk battery item is one
        //      scriptable line instead of an eyeball.
        //
        // Refused at parse time without --nri-graph (the nodes exist only on
        // that path, so it would be a silent no-op -- the same reasoning
        // --golden-stage outside golden mode carries) and without --frames N
        // (the readback lands with frames-in-flight latency, so an open-ended
        // run would never report and would exit on a window close instead).
        bool            pickProbe  = false;
        std::int32_t    pickProbeX = 0;   // canvas px, y-down; only read when pickProbe
        std::int32_t    pickProbeY = 0;
#endif

        // Forward-declared here so it names HostConfig::ParseOutcome and can be the
        // return type of Parse; DEFINED below (after the class closes) because its
        // std::optional<HostConfig> member needs HostConfig to be a complete type,
        // which it is not yet inside this member specification (MSVC enforces this).
        struct ParseOutcome;

        // Builds the Cli spec, parses argv, maps Result -> HostConfig.
        // --help / bad args -> { nullopt, exitCode }; success -> { config, 0 }.
        static ParseOutcome Parse(int argc, char** argv);
    };

    struct HostConfig::ParseOutcome { std::optional<HostConfig> config; int exitCode = 0; };
}
