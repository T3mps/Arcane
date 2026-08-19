#pragma once
// HostConfig: the typed result of parsing a runtime host's command line. Owns
// the host option vocabulary; delegates the parsing to the generic Arcane::Cli
// (Core). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <string>
#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend
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
    // TWO HOSTS READ THIS ENUM NOW (NRI Phase 3, Task 13), and their `Full` is
    // NOT the same picture -- said here because this is shared vocabulary and
    // the difference is otherwise only discoverable in the editor's frame.
    // ArcaneEditor captures the VIEWPORT PANEL, not its window: the editor
    // chrome (dockspace, menus, panels) is excluded by construction, and the
    // plugin's HUD is Play-mode only there. What the editor's `Full` carries
    // INSTEAD of that HUD is the Edit-mode selection outline: its golden run
    // scripts a selection (the same hit-proxy id 1 `--pick-probe` fabricates
    // below -- ArcaneEditor/src/Viewport/GoldenViewPin.hpp) precisely so the
    // stage has content its predecessor does not. `Batch`/`Post` mean the same
    // thing on both -- the same seams, truncated the same way.
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
        // ARTIFACT NAMING (the stem a host derives from this + goldenName).
        // The PREFIX is the host's, not this enum's: ArcaneRuntime's is "main-"
        // and ArcaneEditor's is "editor-" (GoldenHarness.hpp's
        // kEditorGoldenNamePrefix), so the two hosts' artifacts can never
        // overwrite each other and `main-*.png` still means what it always did.
        // Shown here with the runtime's prefix:
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
        // HONOURED ON BOTH RENDER PATHS as of NRI Phase 3, Task 5, and as of
        // Phase 5a (Task 2b) `--crash-gpu N` ALONE reaches the graph dispatch
        // -- the NRI frame graph is the only render path now, so it no longer
        // takes a second flag to select it. `--crash-gpu N` fires
        // NriDiagnostics::FireFault -- the SAME data/shaders/gpu_fault.hlsl
        // TDR loop, as a one-off NRI compute dispatch -- under the same gate,
        // once, with the same `pass:gpu-fault` breadcrumb and the same ERROR
        // text when the injector is unavailable. See RuntimeFrame.cpp's graph
        // arm for the two topology-forced differences (own command buffer;
        // fired before the frame is declared rather than inside it), neither
        // of which is observable in the report. (RenderNvrhi's own
        // --crash-gpu block -- the OTHER arm this paragraph used to describe
        // -- is unreachable now: see RuntimeFrame.cpp's RenderNvrhi.)
        //
        // HISTORY, because a reader may have the old wording in mind: through
        // Phase 2 this flag was a DOCUMENTED NO-OP on `--nri-graph` -- it
        // parsed, never fired, and a run exited 0 having proven nothing. That
        // gap is closed; the deliberate-no-op ruling it carried is history.
        std::uint64_t   crashGpuFrame = 0;

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
        // Refused at parse time without --frames N (the readback lands with
        // frames-in-flight latency, so an open-ended run would never report
        // and would exit on a window close instead). Used to also be refused
        // without --nri-graph -- that guard is gone as of Phase 5a (the NRI
        // frame graph is the only render path, so the pick/outline nodes are
        // always present; the flag itself is retired to a no-op, see below).
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
