#pragma once
// HostConfig: the typed result of parsing a runtime host's command line. Owns
// the host option vocabulary; delegates the parsing to the generic Arcane::Cli
// (Core). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend
namespace Arcane
{
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

        // Print one line of engine-identity JSON to stdout and exit, without creating
        // a window or device. The Arcane Hub probes this to learn the plugin ABI it
        // must stamp into a new .arcproj -- see HostBoot::EngineInfoJson.
        bool            printEngineInfo = false;

        // Render the real frame graph with NO window shown and NO swapchain. Not
        // "--headless": that word already means DEVICE-LESS in this codebase
        // (SceneRenderResolver.hpp:211), and this mode is device-ful.
        //
        // REQUIRES --frames N, refused at parse time otherwise: an unmapped
        // window has no close button and cannot be focused for the `quit`
        // action, so an open-ended offscreen run has no exit of its own. It
        // also REFUSES --pick-probe, which an offscreen context declines to
        // arm. Both refusals carry their full reasoning at the check in the
        // .cpp.
        bool            offscreen = false;

        // Fixed simulation delta, seconds. --offscreen only, refused elsewhere. The
        // host loop is otherwise wall-clock (RuntimeApp.cpp:331), which makes
        // `--frames N` advance the sim by however long those N frames happened to
        // take -- different on a loaded CI box than an idle desk.
        double          fixedDtSeconds = 1.0 / 60.0;

        // Raw, unparsed `--probe` arguments in command-line order. Parsed by
        // VerifyReport, not here: the kinds are that component's vocabulary.
        std::vector<std::string> probes;

        // --settle N. --offscreen only, refused elsewhere -- same idiom as
        // fixedDtSeconds above -- and requires --screenshot or --report too
        // (RuntimeFrame.cpp's CaptureTail has nowhere to land the comparison
        // otherwise, and the loop would never know when to stop). 0 = off.
        //
        // fixedDtSeconds fixes TIME; it does not fix CONTENT. Async shader
        // compiles finish on a WALL-CLOCK schedule the sim clock does not
        // control: ShaderCompiler::Poll's `readyAt` is measured in sim
        // seconds, so DISPATCH is frame-indexed and deterministic, but the
        // compile itself runs on a background thread and completes on real
        // milliseconds. A `--frames N` run landing its capture before that
        // finish shows the untextured fallback; one landing after shows the
        // bound material -- same binary, same flags, back to back (see
        // .superpowers/sdd/2026-08-23-agent-verification-offscreen-hosts/
        // task-6-report.md's bisection: the boundary is exactly 12 frames,
        // 0.2s debounce / (1/60s fixed dt)).
        //
        // N > 1 (>= 2; a bare 1 is refused at parse time -- one attempt has
        // nothing to compare against and would always fail) means: once the
        // ordinary --frames budget above is spent, FREEZE the render/sim
        // clock (RuntimeFrame.cpp's AdvanceSim) and keep re-rendering that
        // same instant -- so nothing but async resource state can still
        // change frame to frame -- capturing again each time, for up to N
        // attempts, until two CONSECUTIVE captures compare BYTE-EQUAL AND
        // the shader compiler reports IsIdle() (fix round 1, item 1 -- byte
        // equality alone is a STABILITY predicate, not the QUIESCENCE one
        // this mode actually needs: Drain() is not clock-gated, only Submit's
        // debounce dispatch is, so two frozen frames can agree while a
        // dispatched-but-undrained compile is still genuinely in flight).
        // Freezing the clock is load-bearing, not an optimisation:
        // ReferenceProject's PulseSprite material shades as a function of
        // Time (Content/materials/pulse_sprite.arcmat), so comparing
        // un-frozen frames would never converge on anything -- every frame
        // would legitimately differ regardless of whether the compile race
        // has resolved.
        //
        // Byte equality, never a tolerance: same mode, same format, same
        // capture path every attempt, so an honest bitwise compare is
        // correct, and a perceptual comparator would hide exactly the
        // failure this flag exists to catch (that is a later plan's job).
        //
        // NON-CONVERGENCE IS A FAILURE, reported explicitly -- exitReason
        // "settle-not-converged" in the JSON report and a nonzero process
        // exit code -- rather than silently handing back whatever the last
        // attempt happened to render. See RuntimeFrame.cpp's CaptureTail and
        // RuntimeApp.cpp's ShutdownGraphPath.
        std::uint64_t settleAttempts = 0;

        // Write the observation report to this JSON path. Empty = off.
        std::string     reportPath = "";

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
        // `--crash-gpu N` ALONE reaches the dispatch -- the NRI frame graph
        // is the only render path, so it takes no second flag to select. It
        // fires NriDiagnostics::FireFault (the data/shaders/gpu_fault.hlsl TDR
        // loop, as a one-off NRI compute dispatch) once, with a
        // `pass:gpu-fault` breadcrumb and an ERROR when the injector is
        // unavailable. See RuntimeFrame.cpp's RenderGraph for the two
        // topology-forced properties -- own command buffer, fired before the
        // frame is declared -- neither of which is observable in the report.
        std::uint64_t   crashGpuFrame = 0;

        // DEV ONLY: `--pick-probe x,y` -- the SCRIPTED desk check for the
        // graph's pick + JFA outline nodes.
        //
        // Turning it on does three things, and all three are the point:
        //   1. the frame grows the pick node (an R32_UINT entity-id target +
        //      a 1-pixel readback) and the outline chain (seed -> N jump-flood
        //      steps -> a composite over the tonemapped backbuffer). With the
        //      flag ABSENT the frame's shape is unchanged, so the probe cannot
        //      perturb what an ordinary run renders;
        //   2. the SELECTION is scripted -- the first pickable drawable in the
        //      scene (hit-proxy id 1) -- because this host has no editor
        //      selection to read;
        //   3. the run prints the id read back at (x, y) and exits 0 on a HIT
        //      (a non-zero id) or 1 on a MISS, so a desk battery item is one
        //      scriptable line instead of an eyeball.
        //
        // Refused at parse time without --frames N (the readback lands with
        // frames-in-flight latency, so an open-ended run would never report
        // and would exit on a window close instead). The pick/outline nodes
        // themselves are always available: the NRI frame graph is the only
        // render path.
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
