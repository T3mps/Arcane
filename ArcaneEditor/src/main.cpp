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
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(argv[0]).c_str());
        return 0;
    }

    // No project and no explicit plugin: refuse rather than boot a project-less
    // session. This used to fall through to a "data/-next-to-exe" boot with no
    // asset registry, no mounts and no identity -- a half-configured editor
    // nothing downstream expects.
    //
    // Both flags stay as bypasses ON PURPOSE: CI and the headless
    // `--project <p> --frames N` harness depend on --project, and --plugin is
    // the engine-dev path (hosting a plugin without a project). Exiting beats a
    // message box here because no window or device exists yet.
    if (parsed.config->projectPath.empty() && parsed.config->pluginPath.empty())
    {
        std::fprintf(stderr,
            "Arcane Editor: no project selected.\n"
            "  Pass --project <folder-or-.arcproj> to open one,\n"
            "  or --plugin <dll> to host a plugin without a project.\n");
        return 2;
    }

    Arcane::Editor::EditorApp app(*parsed.config);
    return app.Run();
}
