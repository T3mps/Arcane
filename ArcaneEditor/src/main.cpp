// Arcane Editor -- the editor shell entry point. Parses argv into a LoomConfig
// (reused as the host config), constructs the EditorApp object, and returns
// its Run() exit code. All engine boot, the frame loop, and the load-bearing
// teardown order live in EditorApp (EditorApp.hpp/.cpp); main is just the
// wire-up (mirrors Loom/src/main.cpp).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <LoomConfig.hpp>
#include <ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)
#include "EditorApp.hpp"

#include <cstdio>

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
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

    Arcane::Editor::EditorApp app(*parsed.config);
    if (noProject)
        app.RaiseOpenProjectOnStart();
    return app.Run();
}
