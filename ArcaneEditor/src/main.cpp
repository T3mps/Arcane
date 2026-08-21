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
//         the rival is focused before returning). This is code 3's ONLY
//         meaning -- nothing post-boot produces it.
//
// THE COLLISION, named rather than left for a reader to rediscover: exit code 2
// means TWO DIFFERENT THINGS depending on WHEN in the run it fires, and
// ArcaneHub's launch.rs decoder (BOOT_WATCHDOG, 2 seconds) reads whichever one
// arrives inside that window. A run that reaches EditorApp::Run() at all has
// already cleared the pre-boot refusals THIS function performs, so from there
// on a 2 means the later thing above -- but the Hub cannot tell the difference
// from the outside, and would misreport a fast one.
//
// NO GUARD CAN CLOSE IT, and launch.rs does not pretend otherwise. The NRI
// frame graph is the only render path, unconditionally, so code 2 can fire on
// ANY ordinary Hub launch that exits inside the 2s watchdog with a validation
// error, and an ordinary launch carries none of the tokens a heuristic could
// key on. So launch.rs's code-2 arm names BOTH meanings -- project gate or
// render error -- rather than confidently reporting an engine/abi refusal that
// was never involved.
//
// ===== WHICH SHARED HostConfig FLAGS THIS HOST ACTUALLY HONOURS ==============
// The table above is exhaustive about EXIT CODES; this is the separate,
// easily-confused question of FLAGS, and it is called out because HostConfig
// is shared by both hosts, so a flag the editor parses is not automatically a
// flag the editor DOES anything with.
//   --project/--plugin/--scene/--frames/--backend/--no-vsync -- honoured.
//   --screenshot        -- honoured; it captures the VIEWPORT panel's texture,
//                          not the editor window.
//   --nri-graph         -- PARSED AND IGNORED: the NRI frame graph is the only
//                          render path, so there is nothing left for this flag
//                          to select.
//   --crash-gpu N       -- honoured (also reachable from the Debug menu).
//   --pick-probe x,y    -- PARSED AND INERT ON THIS HOST. It is a RUNTIME desk
//                          item: one fixed canvas pixel reported as an exit
//                          code. The editor's own pick is a live click through
//                          FrameDesc::pickPixel, and NriGraphContext refuses to
//                          latch the flag on an offscreen (viewport) context
//                          for that reason -- passing it here is a no-op, not a
//                          second pick mechanism.
//   --perf              -- PARSED AND INERT ON THIS HOST. EditorApp constructs
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

    // Post-mortem capture, armed for every REAL run. A crash writes a minidump
    // plus a symbolized all-thread stack; a WEDGED main thread writes the same
    // report while the process is still alive. The second half is the point:
    // Windows Error Reporting only ever fires on process death, so a hang
    // ("Not Responding") otherwise produces nothing, anywhere, ever.
    // Deliberately AFTER the probe -- that path pays for nothing it can skip,
    // and a watchdog thread is not free.
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
    const bool noProject = parsed.config->projectPath.empty() && parsed.config->pluginPath.empty();
    if (noProject && parsed.config->maxFrames != 0)
    {
        std::fprintf(stderr,
            "Arcane Editor: no project selected, and --frames makes this a scripted run.\n"
            "  Pass --project <folder-or-.arcproj> to open one,\n"
            "  or --plugin <dll> to host a plugin without a project.\n");
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
            return 3;
        }
    }

    // Before ANY engine boot: something on screen within ~100ms. Every guard
    // above (the probe, the no-project+--frames refusal, the rival-editor
    // lock check) returns before this line specifically so none of them pays
    // for a window they might not need. Never fails boot -- BootSplashWindow's
    // whole contract is "every error path degrades to no splash, silently".
    Arcane::BootSplashWindow splash("data/images/arcane_logo.png");

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
        Arcane::Editor::EditorApp app(*parsed.config, &splash);
        if (noProject)
            app.RaiseOpenProjectOnStart();
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("editor teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
