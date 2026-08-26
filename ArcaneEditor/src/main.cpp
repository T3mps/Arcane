// Arcane Editor -- the editor shell entry point. Parses argv into a HostConfig
// (shared with ArcaneRuntime), constructs the EditorApp object, and returns
// its Run() exit code. All engine boot, the frame loop, and the load-bearing
// teardown order live in EditorApp (EditorApp.hpp/.cpp); main is just the
// wire-up (mirrors ArcaneRuntime/src/main.cpp).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/Project.hpp>   // EditorLock: the direct-launch double-open guard
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)
#include "App/EditorApp.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>

#ifdef _WIN32
#include <shobjidl.h>
#pragma comment(lib, "shell32.lib")

// The Arcane taskbar GROUP id -- deliberately the family root, not this
// app's own identity. The naming scheme (2026-07-29): dev.starworks.arcane
// is the shared group every Arcane process claims for taskbar stacking;
// per-APP identities hang beneath it (the Hub's installer identity is
// dev.starworks.arcanehub; dev.starworks.arcaneeditor is reserved for this
// editor the day it gains an installer or file-association identity).
// MIRRORED constant: the Hub claims the same group id
// (spawn::APP_USER_MODEL_ID, Arcane/Hub/src-tauri/src/spawn.rs) -- change
// BOTH or the Hub's and the editors' taskbar buttons stop stacking.
static constexpr const wchar_t* kAppUserModelId = L"dev.starworks.arcane";
#endif

// Agility SDK handshake: the D3D12 loader reads these EXPORTED symbols from
// the EXE to redirect device creation into the vendored D3D12Core.dll under
// .\D3D12\. Version must match the vendored package; the proof it took is NRI
// logging "Using ID3D12Device10+".
extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;
extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";

// ===== THE EDITOR'S FULL PROCESS EXIT-CODE TABLE ============================
// Gathered in ONE place. This function's own `return rc;` at the bottom is the
// single point every path below drains through, which is what makes this
// comment honestly exhaustive rather than a claim about code scattered across
// three files.
//
//   0  -- clean exit (including a user quit / closed splash mid-boot).
//   1  -- a boot/init failure (EditorApp::InitResult::Failed), OR a graph
//         frame FAILED, OR GPU device loss. The NRI frame graph is the only
//         render path, so the graph-frame case can fire on ANY run.
//   2  -- PRE-BOOT, from THIS function: no project and no plugin with
//         --frames (a scripted run with nothing to open and nobody to
//         answer a dialog). Also HostConfig::Parse's own refusals (a bad
//         flag, a bad option value) -- those return before main() even
//         reaches this point.
//   2  -- RenderErrorCount GREW across the run, teardown included. Like
//         code 1 it is ungated, so it can fire on any run.
//   3  -- PRE-BOOT, from THIS function: this project is already open in
//         another live editor (the direct-launch double-open guard below;
//         the rival is focused before returning).
//   3  -- (Task 9) --settle N never converged (two consecutive captures
//         never compared byte-equal with the compiler idle, within the
//         attempt budget) -- exitReason "settle-not-converged" or
//         "compare-failed" in the JSON report. Same code RuntimeApp uses
//         for the identical fact (RuntimeApp.cpp's ShutdownGraphPath) --
//         see the collision note below for why this reuses 3 rather than
//         claiming a fresh number.
//   4  -- (Task 9) --compare named a reference that does not exist on disk
//         (or exists but failed to decode) and --bless was not given --
//         exitReason "compare-missing-reference", zero frames rendered
//         (EditorApp::MainLoop's pre-loop resolve-and-fail-fast, mirroring
//         RuntimeApp::MainLoop's own). ALSO --bless converged but failed to
//         WRITE the reference -- exitReason "compare-failed" then. Same
//         code RuntimeApp uses for both facts.
//
// THE COLLISIONS, named rather than left for a reader to rediscover:
//
// Exit code 2 means TWO DIFFERENT THINGS depending on WHEN in the run it
// fires, and ArcaneHub's launch.rs decoder (BOOT_WATCHDOG, 2 seconds) reads
// whichever one arrives inside that window. A run that reaches
// EditorApp::Run() at all has already cleared the pre-boot refusals THIS
// function performs, so from there on a 2 means the later thing above -- but
// the Hub cannot tell the difference from the outside, and would misreport a
// fast one.
//
// NO GUARD CAN CLOSE IT, and launch.rs does not pretend otherwise. The NRI
// frame graph is the only render path, unconditionally, so code 2 can fire on
// ANY ordinary Hub launch that exits inside the 2s watchdog with a validation
// error, and an ordinary launch carries none of the tokens a heuristic could
// key on. So launch.rs's code-2 arm names BOTH meanings -- project gate or
// render error -- rather than confidently reporting an engine/abi refusal that
// was never involved.
//
// Exit code 3 gained a SECOND meaning with Task 9, for the same reason code 2
// already has two: matching RuntimeApp's own m_graphExit numbering (3 =
// settle-not-converged, 4 = compare-missing-reference/bless-write-failed) so
// the two hosts' internal fold logic stays one vocabulary, rather than
// minting editor-only numbers RuntimeApp does not share, outweighs avoiding a
// reuse of 3 the double-open guard already claimed. UNLIKE code 2's
// collision, though, THIS one cannot actually confuse a caller in practice:
// the pre-boot double-open guard fires before Diagnostics::Install, with no
// project ever opened and no report ever requested, while the post-boot
// settle-not-converged code requires a full boot, an --offscreen --settle
// run, and a report/screenshot artifact this task writes -- the two
// preconditions cannot both hold in one invocation, and a caller told which
// command it ran already knows which meaning applies.
//
// ===== WHICH SHARED HostConfig FLAGS THIS HOST ACTUALLY HONOURS ==============
// The table above is exhaustive about EXIT CODES; this is the separate,
// easily-confused question of FLAGS, and it is called out because HostConfig
// is shared by both hosts, so a flag the editor parses is not automatically a
// flag the editor DOES anything with.
//   --project/--plugin/--frames/--backend/--no-vsync -- honoured.
//   --screenshot        -- honoured. WINDOWED it captures the VIEWPORT panel's
//                          texture, not the editor window (so the Inspector and
//                          the asset browser are not in it). Under --offscreen
//                          it captures the COMPOSITED EDITOR FRAME instead --
//                          chrome, docking, panels, and the viewport texture
//                          inside its panel -- off the offscreen chrome
//                          context's colour target (EditorAppFrame.cpp's
//                          PresentChromeFrame).
//   --offscreen         -- honoured. No window is ever mapped and no swapchain
//                          is built anywhere: the chrome context becomes an
//                          OffscreenVehicle (EditorApp::CreateGraphVehicles),
//                          the boot splash below is not constructed at all, and
//                          the per-project ImGui layout is PINNED to the
//                          committed <project>/Saved/verify-layout.ini seed with
//                          io.IniFilename null, so a run can neither read a
//                          machine-local layout nor write one back.
//                          Requires --frames N (HostConfig refuses otherwise).
//   --settle / --report -- HONOURED as of Task 9 (the editor's verification
//   --compare / --bless    surface): PresentChromeFrame's capture arm is the
//                          SAME three-conjunct predicate (byte-equal &&
//                          shader-compiler-idle && [reference-match])
//                          RuntimeFrame::CaptureTail uses, ported rather than
//                          reinvented, and ShutdownGraphPath writes the same
//                          VerifyReport schema (schemaVersion 2) the runtime
//                          does -- see both functions' own comments. The one
//                          divergence from the runtime is structural, not a
//                          gap: this host implements no --probe (below), so
//                          the report's `probes` array is always empty and
//                          there is no `pick` field -- `census` is unaffected,
//                          being carried unconditionally on both hosts.
//   --probe             -- REFUSED at launch on this host, loudly (see below
//                          main()). A scripted caller asking for a pixel/pick
//                          probe would otherwise get exit 0 and a report with
//                          no matching entry -- the same silent-success shape
//                          Task 8 found on the runtime (a --report with pixel
//                          probes and no --screenshot ran, exited 0, and
//                          reported "no capture set" for every probe).
//                          Parsing it and shrugging would be worse than
//                          refusing. ArcaneRuntime honours it.
//   --scene              -- REFUSED at launch on this host (final-fix-wave
//                          audit; this table used to claim it was honoured,
//                          which was FALSE -- HostConfig::sceneOverride has
//                          no consumer anywhere in ArcaneEditor/. The only
//                          hit in this tree is a passing comment in
//                          Project/RuntimeLaunch.hpp; the sole real consumer
//                          is ArcaneRuntime's RuntimeApp.cpp. Without this
//                          refusal, `ArcaneEditor --scene <guid>` parsed,
//                          booted the manifest's default scene, and exited
//                          0 -- silent success for a caller that asked for a
//                          SPECIFIC scene.
//   --fixed-dt           -- REFUSED at launch on this host (final-fix-wave
//                          audit; previously not even mentioned in this
//                          table). The sim clock (EditorAppFrame.cpp's
//                          AdvanceSim) and the chrome/plugin-input clock
//                          (EditorAppFrame.cpp:492-495) are each an
//                          INDEPENDENT std::chrono::steady_clock read --
//                          neither ever looks at fixedDtSeconds. Refusal is
//                          keyed on fixedDtSupplied (HostConfig.hpp), not the
//                          resolved value: the registered default (1/60) is
//                          itself a value a caller could legitimately pass on
//                          purpose, so comparing the resolved double against
//                          that default cannot tell "never passed" from
//                          "passed the default explicitly" apart -- same
//                          r.Supplied() reasoning HostConfig.cpp already uses
//                          for this exact flag internally.
//   --nri-graph         -- PARSED AND IGNORED: the NRI frame graph is the only
//                          render path, so there is nothing left for this flag
//                          to select. THE ONE DELIBERATE EXCEPTION to rule 3
//                          (Task 12 audit) rather than a drift nobody caught:
//                          refusing it would break saved Hub launch args and
//                          scripts that still pass it out of habit, for a flag
//                          that costs nothing to leave parsed-and-ignored. See
//                          HostConfig.cpp's own carve-out comment. Do not
//                          "fix" this one into a refusal.
//   --crash-gpu N       -- honoured (also reachable from the Debug menu).
//   --pick-probe x,y    -- REFUSED at launch on this host (Task 12 audit; see
//                          the refusal below main()). Re-derived rather than
//                          trusted: this comment used to call it a plain
//                          no-op, which undersold it. It IS a no-op from the
//                          caller's side -- nothing ever reports a hit/miss --
//                          but it is not a zero-cost one on a WINDOWED editor.
//                          NriGraphContext.cpp:524's
//                          `m_pickArmed = config.pickProbe && !IsOffscreen()`
//                          reads false only for the OFFSCREEN VIEWPORT
//                          context; the CHROME (host-window) context this
//                          editor also owns is NOT offscreen when windowed, so
//                          the flag DOES arm there -- the pick + JFA outline
//                          NODES get built and one ARC_INFO line gets logged.
//                          What still never happens is the per-frame PASS:
//                          EditorAppFrame.cpp's chrome FrameDesc never sets
//                          `pickOutline`, so AddPickNodes never runs, nothing
//                          is drawn, and no id is ever read back. Net effect
//                          for a caller is unchanged (still nothing useful),
//                          which is why refusing outright is still correct --
//                          it is also a RUNTIME desk item: one fixed canvas
//                          pixel reported as an exit code, and the editor's
//                          own pick is a live click through
//                          FrameDesc::pickPixel instead, so there is nowhere
//                          for this flag to plug in even if it were wired.
//   --perf              -- REFUSED at launch on this host (Task 12 audit; see
//                          the refusal below main()). EditorApp constructs
//                          FramePerf m_perf(m_config.perf) and never calls
//                          FrameStart/Add/Tick -- the ctor and the member
//                          declaration are its ONLY references in this tree, so
//                          no [PERF] line is emitted here by any build. The
//                          runtime drives the same class from RuntimeFrame.cpp
//                          (Tick is the sole emitter). NOT WIRED because
//                          FramePerf's seven fixed buckets (sim/rec/end/tone/
//                          imgui/present/poll) do not map onto this host's
//                          19-phase frame.
// =============================================================================
int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    const Arcane::HostConfig::ParseOutcome parsed = Arcane::HostConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;

    // Probe: identity to stdout, nothing else. Deliberately BEFORE any engine
    // boot -- the Arcane Hub calls this to read the plugin ABI it must stamp
    // into a new .arcproj, and it must not pay for a window, a device, or a
    // registry to answer.
    if (parsed.config->printEngineInfo)
    {
        // ExecutablePathUtf8, NOT argv[0]: argv[0] is whatever the launcher typed
        // (a bare relative name under the documented cd-then-run workflow) and is
        // ANSI-codepage bytes under MSVC, which a strict-UTF-8 dump() rejects.
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(Arcane::ExecutablePathUtf8()).c_str());
        return 0;
    }

    // THE HostConfig FLAGS THIS HOST DOES NOT IMPLEMENT, refused here rather
    // than silently ignored. HostConfig is SHARED with ArcaneRuntime, so
    // every one of these parses cleanly on this exe and would otherwise let
    // a scripted run exit 0 having done nothing useful -- the exact
    // silent-success shape Task 8 paid to find on the runtime (a --report
    // with pixel probes and no --screenshot ran, exited 0, and reported "no
    // capture set" for every probe). An agent reads exit 0 plus a
    // missing/unchanged result as a pass, so the refusal is the fix; wiring
    // any of them into this host is a later task, and it must delete the
    // matching line here on the same day.
    //
    // TASK 9 DELETES --report AND --settle FROM THIS CONDITION -- exactly
    // the "delete the matching line here on the same day" the comment above
    // promised. --compare/--bless need no line of their own: HostConfig.cpp
    // refuses either one without --settle already (a shared, parse-time
    // gate), so lifting --settle transitively lifts them too. --probe STAYS
    // REFUSED, on its own condition below now rather than folded into this
    // one: it is not this task's title, and ArcaneEditor implements no probe
    // evaluation at all -- folding a three-flag OR into one message was
    // exactly what made it easy to delete two-thirds of it without noticing
    // the third had to stay.
    //
    // AHEAD OF Diagnostics::Install BELOW, and that placement is load-bearing
    // rather than tidy: an early `return` taken AFTER Install leaves the hang
    // watchdog's std::thread joinable at static destruction, which is
    // std::terminate -> abort(). The two refusals further down USED to pay
    // for exactly this (a bare `--frames 10` with no project was documented
    // as exit 2 and, this task discovered by actually running it, does not
    // even reliably reach exit code 3 -- under a Debug CRT abort() pops a
    // BLOCKING "Microsoft Visual C++ Runtime Library" dialog and the process
    // never exits on its own; see those two sites for the measurement). That
    // turned out to be worse than a documentation mismatch, so THIS task
    // fixed both of them too, by calling Diagnostics::Shutdown() before their
    // `return`s rather than moving them -- zero behaviour cost, and it does
    // not touch ArcaneHub's exit-code contract for either one. The refusal
    // below (--probe) still goes ahead of Install instead, matching this
    // block's existing shape rather than adding a third flavour of fix to
    // one function.
    if (!parsed.config->probes.empty())
    {
        std::fprintf(stderr,
            "Arcane Editor: --probe is not implemented on this host.\n"
            "  This exe would exit 0 having produced no probe entries, which is worse than\n"
            "  refusing. Use ArcaneRuntime for pixel/pick probes; ArcaneEditor supports\n"
            "  --offscreen --frames N --settle N --report <json> --screenshot <png>\n"
            "  --compare <name> [--bless] for a composited editor capture and verification.\n");
        return 2;
    }
#if !defined(ARCANE_DIST)
    // --pick-probe: DEV-ONLY, matching HostConfig.hpp/.cpp's own
    // #if !defined(ARCANE_DIST) guard around the pickProbe member and its Cli
    // registration -- the member does not exist on a Dist build, so reading
    // parsed.config->pickProbe unguarded would fail to COMPILE there, not just
    // misbehave (confirmed by building Dist below). Same reasoning and same
    // position as the block above: this is genuinely NOT a zero-cost no-op on
    // this host -- see the disposition table above main() -- NriGraphContext
    // arms the pick+outline nodes on the windowed chrome context and logs one
    // line -- but nothing ever reads an id back, so the caller still gets
    // nothing for its trouble. Refuse rather than let that drift stand.
    if (parsed.config->pickProbe)
    {
        std::fprintf(stderr, "error: --pick-probe is a RUNTIME flag; the editor's pick is a live "
                             "click through FrameDesc::pickPixel. Use ArcaneRuntime.exe.\n");
        return 2;
    }
#endif
    // --perf: same reasoning and position as above (and its own #if is not
    // needed -- HostConfig::perf is NOT Dist-guarded, unlike pickProbe).
    // EditorApp constructs FramePerf m_perf(m_config.perf) and never calls
    // FrameStart/Add/Tick, so no [PERF] line is ever emitted on this host by
    // any build -- FramePerf's seven fixed buckets do not map onto this
    // host's 19-phase frame.
    if (parsed.config->perf)
    {
        std::fprintf(stderr, "error: --perf is not implemented on the editor host "
                             "(FramePerf is constructed and never sampled). Use ArcaneRuntime.exe.\n");
        return 2;
    }

    // --fixed-dt: final-fix-wave audit finding, same reasoning and position as
    // --perf/--pick-probe above. AdvanceSim (EditorAppFrame.cpp) and the
    // chrome/plugin-input dt a few lines above it are each an independent
    // std::chrono::steady_clock read; neither ever consults fixedDtSeconds, so
    // this exe would otherwise exit 0 having silently run wall-clock anyway.
    // Gated on fixedDtSupplied, not the resolved value -- see HostConfig.hpp's
    // comment on that field for why comparing the resolved double against its
    // own default cannot tell "never passed" from "passed the default
    // explicitly" apart.
    if (parsed.config->fixedDtSupplied)
    {
        std::fprintf(stderr, "error: --fixed-dt is not implemented on the editor host "
                             "(the sim and chrome clocks are both wall-clock steady_clock reads that "
                             "never consult it). Use ArcaneRuntime.exe for a deterministic fixed "
                             "timestep.\n");
        return 2;
    }
    // --scene: final-fix-wave audit finding. HostConfig::sceneOverride has no
    // consumer anywhere in this tree -- ArcaneRuntime's RuntimeApp.cpp is the
    // only real reader. Without this refusal `ArcaneEditor --scene <guid>`
    // parsed cleanly, booted the manifest's default scene, and exited 0: a
    // silent no-op for a caller that asked for a specific scene.
    if (!parsed.config->sceneOverride.empty())
    {
        std::fprintf(stderr, "error: --scene is not implemented on the editor host "
                             "(this exe always boots the project manifest's bootScene). Use "
                             "ArcaneRuntime.exe for a scene override, or open the scene from the "
                             "editor's asset browser once booted.\n");
        return 2;
    }

    // Post-mortem capture, armed for every REAL run. A crash writes a minidump
    // plus a symbolized all-thread stack; a WEDGED main thread writes the same
    // report while the process is still alive. The second half is the point:
    // Windows Error Reporting only ever fires on process death, so a hang
    // ("Not Responding") otherwise produces nothing, anywhere, ever.
    // Deliberately AFTER the probe -- that path pays for nothing it can skip,
    // and a watchdog thread is not free.
    //
    // ORDERING IS LOAD-BEARING, reciprocal note to the block above: every flag
    // refusal above this point `return`s before the watchdog thread exists, so
    // each one is a clean exit at its documented code. Moving this Install()
    // call any earlier turns every one of those returns into std::terminate ->
    // abort() -> exit code 3 instead -- measured, not theorised (see above).
    // Do not move this line up without re-auditing every refusal above it.
    {
        Arcane::Diagnostics::Config diag;
        diag.appName = "ArcaneEditor";
        Arcane::Diagnostics::Install(diag);
    }

#ifdef _WIN32
    // One taskbar family. Windows groups taskbar buttons by AppUserModelID,
    // NOT by process parentage, so the editor claims the same id the Arcane
    // Hub sets and their buttons stack under one group -- whether this
    // process was spawned by the Hub or double-clicked directly. Must run
    // before the first window exists, hence here and not in EditorApp.
    // Deliberately unconditional rather than an --appid flag: a flag would
    // make an older editor build reject the unknown argument at launch.
    // AFTER the probe on purpose -- the probe pays for nothing it can skip.
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
#endif

    // No project and no explicit plugin. A SCRIPTED run (--frames N) still
    // refuses: it cannot answer a dialog, and booting project-less used to mean a
    // "data/-next-to-exe" session with no asset registry, no mounts and no
    // identity -- a half-configured editor nothing downstream expects.
    //
    // An INTERACTIVE bare launch does NOT refuse. It boots and immediately raises
    // the shipped File -> Open Project dialog (EditorApp::m_raiseOpenProjectOnStart).
    // Exiting here removed the only cold-start path into that dialog, even though
    // the project-less state is explicitly supported everywhere else -- see
    // SwitchProject's failure path: "editor left with no plugin; user can Open
    // another project". The Arcane Hub is the normal entry point and always passes
    // --project; this is the fallback for anyone who runs the exe directly.
    //
    // Both flags remain bypasses ON PURPOSE: CI and the headless
    // `--project <p> --frames N` harness depend on --project, and --plugin is the
    // engine-dev path (hosting a plugin without a project).
    // Task 12 fix, MEASURED rather than left as the theorised "actually exits
    // 3" the comment above once claimed: under a Debug CRT this `return 2`,
    // taken after Diagnostics::Install armed the watchdog thread, does not
    // even reach exit code 3 cleanly -- std::terminate's default handler pops
    // a BLOCKING "Microsoft Visual C++ Runtime Library" MessageBox
    // (confirmed via Process.MainWindowTitle on a run of exactly this
    // command) and the process never exits on its own. That is a hang, not a
    // wrong code, and a scripted/CI caller has no recourse but an external
    // timeout-kill. Shutdown() joins the watchdog cleanly first, so this
    // `return 2` is now an ordinary clean exit at the code it names --
    // zero behaviour cost otherwise, and it needed no ordering change.
    const bool noProject = parsed.config->projectPath.empty() && parsed.config->pluginPath.empty();
    if (noProject && parsed.config->maxFrames != 0)
    {
        std::fprintf(stderr,
            "Arcane Editor: no project selected, and --frames makes this a scripted run.\n"
            "  Pass --project <folder-or-.arcproj> to open one,\n"
            "  or --plugin <dll> to host a plugin without a project.\n");
        Arcane::Diagnostics::Shutdown();
        return 2;
    }

    // The direct-launch guard: refuse to double-open a project another LIVE
    // editor already holds, and bring that editor forward instead. This is
    // the one multi-open path the Hub cannot see -- its own launches take
    // the same lock through editorlock.rs, but nothing stops two direct
    // `ArcaneEditor --project X` invocations except this. Root derivation
    // mirrors Project::Open (a .arcproj names its parent; anything else IS
    // the folder), RivalPid is self-exempt and defeats stale locks by
    // pid+creation-time, and exit code 3 is distinct from 2 (the no-project
    // refusal above) so the Hub's boot watchdog can name the reason.
    //
    // Same Shutdown()-before-return fix as the no-project block above, and
    // the same `return`-after-`Install()` shape just MEASURED there -- but
    // NOT independently measured for this specific branch: reproducing it
    // needs a second, already-live rival editor process holding the lock,
    // and standing one up risks exactly the windowed boot this task's
    // verification avoids. Applying the identical fix on the strength of the
    // identical mechanism, not a second measurement of this exact path.
    if (!parsed.config->projectPath.empty())
    {
        std::filesystem::path lockRoot(parsed.config->projectPath);
        if (lockRoot.extension() == ".arcproj")
            lockRoot = lockRoot.parent_path();
        if (const auto rival = Arcane::EditorLock::RivalPid(lockRoot))
        {
            std::fprintf(stderr,
                "Arcane Editor: '%s' is already open in another editor (pid %u) -- focusing it.\n",
                parsed.config->projectPath.c_str(), *rival);
            Arcane::EditorLock::FocusWindowOfProcess(*rival);
            Arcane::Diagnostics::Shutdown();
            return 3;
        }
    }

    // Before ANY engine boot: something on screen within ~100ms. Every guard
    // above (the probe, the no-project+--frames refusal, the rival-editor
    // lock check) returns before this line specifically so none of them pays
    // for a window they might not need. Never fails boot -- BootSplashWindow's
    // whole contract is "every error path degrades to no splash, silently".
    // ...UNLESS --offscreen, where the whole point is that this process maps
    // no window. The splash is a real WS_POPUP on its own thread, so
    // constructing it unconditionally would make "--offscreen" a lie for the
    // ~seconds boot takes -- the one window an offscreen run would still
    // flash on screen. std::optional is what it takes: BootSplashWindow's
    // only constructor takes an image path and there is no "disabled" state,
    // and every consumer downstream is already null-tolerant by contract
    // (BootSplashPresenter's ctor comment: "`splash` may be null ... every
    // call then degrades to do nothing"; EditorApp guards with `if
    // (m_splash)`). Destruction order is unchanged -- `app` still lives in the
    // nested scope below and is destroyed before this object.
    std::optional<Arcane::BootSplashWindow> splash;
    if (!parsed.config->offscreen)
        splash.emplace("data/images/arcane_logo.png");

    // Scoped so ~EditorApp -- the load-bearing teardown sequence -- runs while
    // the watchdog is STILL armed. Teardown does not beat, so a deadlock in it
    // reports as a hang, which is exactly right: tearing down threaded state is
    // itself a suspect. Shutdown() then joins the watchdog before main returns;
    // leaving a joinable std::thread to static destruction would call
    // std::terminate and turn a clean exit into a crash.
    // Declaring app in a nested scope keeps its destruction BEFORE splash's,
    // the same relative order as when both were siblings here.
    int rc = 0;
    {
        Arcane::Editor::EditorApp app(*parsed.config, splash ? &*splash : nullptr);
        if (noProject)
            app.RaiseOpenProjectOnStart();
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("editor teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
