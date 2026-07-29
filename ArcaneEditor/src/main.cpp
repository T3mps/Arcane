// Arcane Editor -- the editor shell entry point. Parses argv into a HostConfig
// (shared with ArcaneRuntime), constructs the EditorApp object, and returns
// its Run() exit code. All engine boot, the frame loop, and the load-bearing
// teardown order live in EditorApp (EditorApp.hpp/.cpp); main is just the
// wire-up (mirrors ArcaneRuntime/src/main.cpp).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/Project.hpp>   // EditorLock: the direct-launch double-open guard
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)
#include "EditorApp.hpp"

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

    Arcane::Editor::EditorApp app(*parsed.config);
    if (noProject)
        app.RaiseOpenProjectOnStart();
    return app.Run();
}
